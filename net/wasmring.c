/*
 * wasmring netdev: an Ethernet backend whose wire is a pair of wasm-ring
 * record rings shared with JavaScript (see include/qemu/wasm-ring.h).
 *
 *   -netdev wasmring,id=n0 -device virtio-net-pci,netdev=n0
 *
 * Frames the guest sends become records in the "out" ring; records JS puts
 * in the "in" ring are delivered to the guest from a main-loop timer, only
 * while the NIC can take them (the rest wait in the ring, like a wire with
 * a buffer). It replaces the socket netdev, which needs Emscripten SOCKFS
 * and therefore the proxied JS filesystem.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "net/net.h"
#include "clients.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/wasm-ring.h"

#ifdef __EMSCRIPTEN__

#define WASMRING_NET_CAP     (1u << 20)
#define WASMRING_NET_POLL_MS 2
#define WASMRING_NET_MAXFRAME 65536

typedef struct WasmRingNetState {
    NetClientState nc;
    WasmRing *in, *out;
    QEMUTimer *poll;
    uint8_t buf[WASMRING_NET_MAXFRAME];
} WasmRingNetState;

static ssize_t wasmring_net_receive(NetClientState *nc, const uint8_t *buf,
                                    size_t size)
{
    WasmRingNetState *s = DO_UPCAST(WasmRingNetState, nc, nc);

    /* The page stopped draining: drop (counted), as a full wire would. */
    wasm_ring_put_record(s->out, buf, size);
    return size;
}

static void wasmring_net_poll(void *opaque)
{
    WasmRingNetState *s = opaque;

    while (qemu_can_send_packet(&s->nc)) {
        int n = wasm_ring_get_record(s->in, s->buf, sizeof(s->buf));

        if (n == -1) {
            break;
        }
        if (n >= 0) {
            qemu_send_packet(&s->nc, s->buf, n);
        }
    }
    timer_mod(s->poll,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + WASMRING_NET_POLL_MS);
}

static void wasmring_net_cleanup(NetClientState *nc)
{
    WasmRingNetState *s = DO_UPCAST(WasmRingNetState, nc, nc);

    if (s->poll) {
        timer_free(s->poll);
        s->poll = NULL;
    }
    /* The rings stay allocated: JS may still hold their addresses. */
}

static NetClientInfo net_wasmring_info = {
    .type = NET_CLIENT_DRIVER_WASMRING,
    .size = sizeof(WasmRingNetState),
    .receive = wasmring_net_receive,
    .cleanup = wasmring_net_cleanup,
};

int net_init_wasmring(const Netdev *netdev, const char *name,
                      NetClientState *peer, Error **errp)
{
    static int index;
    NetClientState *nc;
    WasmRingNetState *s;

    assert(netdev->type == NET_CLIENT_DRIVER_WASMRING);
    nc = qemu_new_net_client(&net_wasmring_info, peer, "wasmring", name);
    s = DO_UPCAST(WasmRingNetState, nc, nc);
    s->in = wasm_ring_new(WASMRING_NET_CAP);
    s->out = wasm_ring_new(WASMRING_NET_CAP);
    s->poll = timer_new_ms(QEMU_CLOCK_REALTIME, wasmring_net_poll, s);
    timer_mod(s->poll, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    qemu_set_info_str(nc, "wasmring");
    wasm_ring_announce(WASM_RING_KIND_NET, index++, s->in, s->out);
    return 0;
}

#else /* !__EMSCRIPTEN__ */

int net_init_wasmring(const Netdev *netdev, const char *name,
                      NetClientState *peer, Error **errp)
{
    error_setg(errp, "the wasmring netdev exists only in Emscripten builds");
    return -1;
}

#endif
