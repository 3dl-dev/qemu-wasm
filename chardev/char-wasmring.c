/*
 * wasmring chardev: a character device backed by two wasm-ring byte rings
 * shared with JavaScript (see include/qemu/wasm-ring.h for why).
 *
 *   -chardev wasmring,id=con
 *
 * Guest output goes to the "out" ring as it is written; input the page puts
 * in the "in" ring is fed to the frontend from a main-loop timer. Neither
 * side ever blocks on the other.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/wasm-ring.h"
#include "qapi/error.h"

#define WASMRING_CHR_CAP     (1u << 20)
#define WASMRING_CHR_POLL_MS 5

typedef struct {
    Chardev parent;
    WasmRing *in, *out;
    QEMUTimer *poll;
} WasmRingChardev;

#define TYPE_CHARDEV_WASMRING "chardev-wasmring"
DECLARE_INSTANCE_CHECKER(WasmRingChardev, WASMRING_CHARDEV,
                         TYPE_CHARDEV_WASMRING)

static int wasmring_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    WasmRingChardev *d = WASMRING_CHARDEV(chr);

    /*
     * A console has nowhere to park output; if the page stopped draining a
     * megabyte, the overflow is dropped and counted in the ring's drops.
     */
    wasm_ring_write(d->out, buf, len);
    return len;
}

#ifdef __EMSCRIPTEN__
static void wasmring_chr_poll(void *opaque)
{
    WasmRingChardev *d = opaque;
    Chardev *chr = CHARDEV(d);
    uint8_t buf[256];

    for (;;) {
        int room = qemu_chr_be_can_write(chr);
        uint32_t n;

        if (room <= 0) {
            break;
        }
        n = wasm_ring_read(d->in, buf, MIN(room, (int)sizeof(buf)));
        if (!n) {
            break;
        }
        qemu_chr_be_write(chr, buf, n);
    }
    timer_mod(d->poll,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + WASMRING_CHR_POLL_MS);
}

static void wasmring_chr_open(Chardev *chr, ChardevBackend *backend,
                              bool *be_opened, Error **errp)
{
    static int index;
    WasmRingChardev *d = WASMRING_CHARDEV(chr);

    d->in = wasm_ring_new(WASMRING_CHR_CAP);
    d->out = wasm_ring_new(WASMRING_CHR_CAP);
    d->poll = timer_new_ms(QEMU_CLOCK_REALTIME, wasmring_chr_poll, d);
    timer_mod(d->poll, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    wasm_ring_announce(WASM_RING_KIND_CHARDEV, index++, d->in, d->out);
    *be_opened = true;
}
#else
static void wasmring_chr_open(Chardev *chr, ChardevBackend *backend,
                              bool *be_opened, Error **errp)
{
    error_setg(errp, "the wasmring chardev exists only in Emscripten builds");
}
#endif

static void wasmring_chr_finalize(Object *obj)
{
    WasmRingChardev *d = WASMRING_CHARDEV(obj);

    if (d->poll) {
        timer_free(d->poll);
        d->poll = NULL;
    }
    /* The rings stay allocated: JS may still hold their addresses. */
}

static void char_wasmring_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open = wasmring_chr_open;
    cc->chr_write = wasmring_chr_write;
}

static const TypeInfo char_wasmring_type_info = {
    .name = TYPE_CHARDEV_WASMRING,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(WasmRingChardev),
    .instance_finalize = wasmring_chr_finalize,
    .class_init = char_wasmring_class_init,
};

static void register_types(void)
{
    type_register_static(&char_wasmring_type_info);
}

type_init(register_types);
