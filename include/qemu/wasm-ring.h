/*
 * wasm-ring: a single-producer/single-consumer byte ring in guest-process
 * memory, shared with JavaScript on another thread through the Wasm memory
 * SharedArrayBuffer.
 *
 * Why this exists (qemu-wasm, OpenVMX rd vms-e79): with Emscripten's JS
 * filesystem every file, socket and tty syscall a QEMU pthread makes is
 * proxied, synchronously, to the runtime ("main") thread. When that thread
 * is busy or parked, disk completions, the main loop's poll and the
 * console all stall, and the emulator freezes. A WASMFS build keeps file
 * I/O inside Wasm on the calling thread, but WASMFS has no sockets and no
 * pollable stdin, so the console and the NIC move onto these rings. The JS
 * side only ever touches the ring with Atomics on the shared heap; it never
 * calls into Wasm and QEMU never waits for it.
 *
 * Layout (all little-endian u32, byte offsets from the ring pointer):
 *   0  head   total bytes ever written (producer advances, release)
 *   4  tail   total bytes ever read (consumer advances, release)
 *   8  cap    data capacity in bytes, a power of two
 *  12  drops  bytes or records the producer discarded because it was full
 *  16  data[cap]
 * Record rings (the NIC) carry [u32 length][bytes] with the same wrap rule.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QEMU_WASM_RING_H
#define QEMU_WASM_RING_H

#include "qemu/atomic.h"
#include "qemu/bswap.h"

typedef struct WasmRing {
    uint32_t head;
    uint32_t tail;
    uint32_t cap;
    uint32_t drops;
    uint8_t data[];
} WasmRing;

/* Ring kinds announced to JavaScript (Module.ovmxRing(kind, ...)). */
#define WASM_RING_KIND_CHARDEV 1
#define WASM_RING_KIND_NET     2

static inline WasmRing *wasm_ring_new(uint32_t cap)
{
    WasmRing *r;

    assert(cap && !(cap & (cap - 1)));
    r = g_malloc0(sizeof(*r) + cap);
    r->cap = cap;
    return r;
}

static inline uint32_t wasm_ring_used(WasmRing *r)
{
    return qatomic_load_acquire(&r->head) - qatomic_load_acquire(&r->tail);
}

static inline void wasm_ring_copy_in(WasmRing *r, uint32_t pos,
                                     const uint8_t *buf, uint32_t len)
{
    uint32_t off = pos & (r->cap - 1);
    uint32_t first = MIN(len, r->cap - off);

    memcpy(r->data + off, buf, first);
    memcpy(r->data, buf + first, len - first);
}

static inline void wasm_ring_copy_out(WasmRing *r, uint32_t pos,
                                      uint8_t *buf, uint32_t len)
{
    uint32_t off = pos & (r->cap - 1);
    uint32_t first = MIN(len, r->cap - off);

    memcpy(buf, r->data + off, first);
    memcpy(buf + first, r->data, len - first);
}

/* Producer: write up to len bytes; returns the number written. */
static inline uint32_t wasm_ring_write(WasmRing *r, const uint8_t *buf,
                                       uint32_t len)
{
    uint32_t head = qatomic_read(&r->head);
    uint32_t space = r->cap - (head - qatomic_load_acquire(&r->tail));
    uint32_t n = MIN(len, space);

    wasm_ring_copy_in(r, head, buf, n);
    qatomic_store_release(&r->head, head + n);
    if (n < len) {
        qatomic_add(&r->drops, len - n);
    }
    return n;
}

/* Consumer: read up to len bytes; returns the number read. */
static inline uint32_t wasm_ring_read(WasmRing *r, uint8_t *buf, uint32_t len)
{
    uint32_t tail = qatomic_read(&r->tail);
    uint32_t avail = qatomic_load_acquire(&r->head) - tail;
    uint32_t n = MIN(len, avail);

    wasm_ring_copy_out(r, tail, buf, n);
    qatomic_store_release(&r->tail, tail + n);
    return n;
}

/* Producer: write one whole record, or drop it (counted) if it does not fit. */
static inline bool wasm_ring_put_record(WasmRing *r, const uint8_t *buf,
                                        uint32_t len)
{
    uint32_t head = qatomic_read(&r->head);
    uint32_t space = r->cap - (head - qatomic_load_acquire(&r->tail));
    uint32_t hdr = cpu_to_le32(len);

    if (space < sizeof(hdr) + len) {
        qatomic_inc(&r->drops);
        return false;
    }
    wasm_ring_copy_in(r, head, (const uint8_t *)&hdr, sizeof(hdr));
    wasm_ring_copy_in(r, head + sizeof(hdr), buf, len);
    qatomic_store_release(&r->head, head + sizeof(hdr) + len);
    return true;
}

/*
 * Consumer: take one whole record into buf. Returns its length, -1 if the
 * ring holds no complete record. A record longer than bufsize is consumed
 * and discarded (counted in drops) and -2 is returned.
 */
static inline int wasm_ring_get_record(WasmRing *r, uint8_t *buf,
                                       uint32_t bufsize)
{
    uint32_t tail = qatomic_read(&r->tail);
    uint32_t avail = qatomic_load_acquire(&r->head) - tail;
    uint32_t hdr, len;

    if (avail < sizeof(hdr)) {
        return -1;
    }
    wasm_ring_copy_out(r, tail, (uint8_t *)&hdr, sizeof(hdr));
    len = le32_to_cpu(hdr);
    if (avail < sizeof(hdr) + len) {
        return -1;
    }
    if (len > bufsize) {
        qatomic_inc(&r->drops);
        qatomic_store_release(&r->tail, tail + sizeof(hdr) + len);
        return -2;
    }
    wasm_ring_copy_out(r, tail + sizeof(hdr), buf, len);
    qatomic_store_release(&r->tail, tail + sizeof(hdr) + len);
    return len;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/*
 * Tell the JS runtime thread where a ring pair lives. Fire-and-forget: the
 * call is queued to the runtime thread and QEMU does not wait for it.
 * Module.ovmxRing(kind, index, inRing, outRing, buffer) -- "in" is JS->QEMU,
 * "out" is QEMU->JS; buffer is the shared Wasm memory.
 */
static inline void wasm_ring_announce(int kind, int index, WasmRing *in,
                                      WasmRing *out)
{
    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module['ovmxRing']) {
            Module['ovmxRing']($0, $1, $2, $3, HEAPU8.buffer);
        }
    }, kind, index, in, out);
}
#endif

#endif /* QEMU_WASM_RING_H */
