#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "input.h"
#include "bootinfo.h"
#include "serial.h"

/* Rings live outside the kernel image so the image still fits below 0x70000.
 * The UEFI loader identity-maps this range. */
#define XHCI_HEAP ((uint8_t *)0x1100000)
#define XHCI_HEAP_SIZE (384u * 1024u)

typedef struct {
    uint32_t d[4];
} Trb;

typedef struct {
    Trb *trb;
    uint32_t enqueue;
    uint32_t dequeue;
    uint32_t cycle;
    uint32_t size;
} Ring;

static uint8_t *heap;
static uint32_t heap_used;
static volatile uint8_t *cap;
static volatile uint32_t *op;
static volatile uint32_t *db;
static volatile uint32_t *rt;
static uint32_t ctx_size;
static uint64_t *dcbaa;
static Ring cmd;
static Ring event;
static int ready;

typedef struct {
    int slot;
    int dci;
    int kind; /* 1 keyboard, 2 mouse */
    int iface;
    Ring ring;
    uint8_t report[16];
    uint8_t prev[16];
    uint32_t mps;
    Trb *posted;
} HidEp;

static HidEp eps[4];
static int ep_count;
static char status_line[48] = "No USB chip";
static int saw_controller, saw_ports, saw_devs, saw_hubs, last_err;

const char *xhci_status_line(void) { return status_line; }

static void set_status(const char *text);

void xhci_set_status_line(const char *text) { set_status(text); }

void xhci_append_status(const char *extra) {
    int i = 0;
    while (status_line[i] && i < (int)sizeof(status_line) - 1) i++;
    for (int k = 0; extra[k] && i < (int)sizeof(status_line) - 1; k++) status_line[i++] = extra[k];
    status_line[i] = 0;
}

static void set_status(const char *text) {
    int i = 0;
    for (; text[i] && i < (int)sizeof(status_line) - 1; i++) status_line[i] = text[i];
    status_line[i] = 0;
}

static void note_status(void) {
    int keyboard = 0;
    for (int i = 0; i < ep_count; i++) if (eps[i].kind == 1) keyboard = 1;
    if (keyboard) {
        set_status("USB keyboard ready");
        return;
    }
    char text[48];
    char *p = text;
    const char *prefix = "USB p";
    while (*prefix) *p++ = *prefix++;
    *p++ = (char)('0' + (saw_ports > 9 ? 9 : saw_ports));
    const char *mid = " d";
    while (*mid) *p++ = *mid++;
    *p++ = (char)('0' + (saw_devs > 9 ? 9 : saw_devs));
    mid = " h";
    while (*mid) *p++ = *mid++;
    *p++ = (char)('0' + (saw_hubs > 9 ? 9 : saw_hubs));
    if (last_err) {
        mid = " e";
        while (*mid) *p++ = *mid++;
        int e = last_err < 0 ? -last_err : last_err;
        if (e > 9) *p++ = (char)('0' + e / 10);
        *p++ = (char)('0' + e % 10);
    }
    *p = 0;
    set_status(text);
}

static int enumerate_device(int root_port, int speed, uint32_t route, int parent_slot,
                            int parent_port, uint32_t slots, int depth, int hub_mtt, int think);

static void *halloc(uint32_t size, uint32_t align) {
    uint32_t a = (heap_used + align - 1u) & ~(align - 1u);
    if (a + size > XHCI_HEAP_SIZE) return 0;
    heap_used = a + size;
    return heap + a;
}

static void flush_range(void *p, uint32_t n);

static uint32_t rd(volatile uint32_t *r) { return *r; }
static void wr(volatile uint32_t *r, uint32_t v) { *r = v; }
static void wr64(volatile uint32_t *r, uint32_t low) {
    r[0] = low;
    r[1] = 0;
}

static void wait_ms(int ms) {
    uint32_t need = (uint32_t)(ms / 10);
    if (!need) need = 1;
    uint32_t start = timer_ticks();
    while (timer_ticks() - start < need) input_timer();
}

static int ring_init(Ring *r, uint32_t n, int with_link) {
    uint32_t count = with_link ? n + 1 : n;
    r->trb = halloc(count * sizeof(Trb), 64);
    if (!r->trb) return -1;
    memset(r->trb, 0, count * sizeof(Trb));
    r->size = n;
    r->enqueue = 0;
    r->dequeue = 0;
    r->cycle = 1;
    if (with_link) {
        r->trb[n].d[0] = (uint32_t)(uintptr_t)r->trb;
        r->trb[n].d[3] = (6u << 10) | (1u << 1) | 1u;
    }
    return 0;
}

static Trb *ring_push(Ring *r, uint32_t a, uint32_t b, uint32_t c, uint32_t flags) {
    Trb *t = &r->trb[r->enqueue];
    t->d[0] = a;
    t->d[1] = b;
    t->d[2] = c;
    t->d[3] = flags | r->cycle;
    r->enqueue++;
    if (r->enqueue == r->size) {
        r->trb[r->size].d[0] = (uint32_t)(uintptr_t)r->trb;
        r->trb[r->size].d[1] = 0;
        r->trb[r->size].d[2] = 0;
        r->trb[r->size].d[3] = (6u << 10) | (1u << 1) | r->cycle;
    r->enqueue = 0;
    r->cycle ^= 1u;
    }
    flush_range(t, sizeof(*t));
    return t;
}

static void erdp_update(void) {
    uint32_t next = (uint32_t)(uintptr_t)&event.trb[event.dequeue];
    wr64(rt + (0x38 / 4), next | 8u);
}

static int event_poll(Trb *out) {
    Trb *t = &event.trb[event.dequeue];
    flush_range(t, sizeof(*t));
    if ((t->d[3] & 1u) != event.cycle) return 0;
    *out = *t;
    event.dequeue++;
    if (event.dequeue == event.size) {
        event.dequeue = 0;
        event.cycle ^= 1u;
    }
    erdp_update();
    return 1;
}

static void flush_range(void *p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~63u;
    uintptr_t end = ((uintptr_t)p + n + 63u) & ~63u;
    for (; a < end; a += 64)
        __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

static void doorbell(int slot, uint32_t target) {
    db[slot] = target;
}

static int wait_event(uint32_t type, uint32_t ptr, uint32_t *code, uint32_t *slot) {
    for (int i = 0; i < 300; i++) {
        Trb ev;
        if (!event_poll(&ev)) {
            wait_ms(10);
            continue;
        }
        uint32_t got = (ev.d[3] >> 10) & 0x3Fu;
        if (got != type) continue;
        if (ptr && ev.d[0] != ptr) continue;
        if (code) *code = ev.d[2] >> 24;
        if (slot) *slot = ev.d[3] >> 24;
        return 0;
    }
    return -1;
}

static int control(Ring *ep0, int slot, uint8_t bm, uint8_t req, uint16_t val,
                   uint16_t idx, void *data, uint16_t len, int dir_in, uint32_t *code) {
    uint32_t setup_lo = (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t setup_hi = (uint32_t)idx | ((uint32_t)len << 16);
    uint32_t trt = len ? (dir_in ? 3u : 2u) : 0;
    /* Bit 6 (IDT) puts the 8-byte setup packet in this TRB. Controllers
     * and QEMU both require it. */
    ring_push(ep0, setup_lo, setup_hi, 8, (2u << 10) | (trt << 16) | (1u << 4) | (1u << 6));
    if (len) {
        flush_range(data, len);
        ring_push(ep0, (uint32_t)(uintptr_t)data, 0, len,
                  (3u << 10) | (1u << 4) | (dir_in ? (1u << 16) : 0));
    }
    ring_push(ep0, 0, 0, 0,
              (4u << 10) | (1u << 5) | ((len && dir_in) ? 0 : (1u << 16)));
    flush_range(ep0->trb, (ep0->size + 1u) * sizeof(Trb));
    doorbell(slot, 1);
    for (int i = 0; i < 100; i++) {
        Trb ev;
        if (!event_poll(&ev)) {
            wait_ms(10);
            continue;
        }
        uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
        if (type != 32) continue;
        uint32_t got = ev.d[2] >> 24;
        if (code) *code = got;
        if ((got == 1 || got == 13) && dir_in && data && len) flush_range(data, len);
        return (got == 1 || got == 13) ? 0 : -1;
    }
    if (code) *code = 0;
    return -1;
}

static void bios_handoff(void) {
    uint32_t hcc = rd((volatile uint32_t *)(cap + 0x10));
    uint32_t xecp = (hcc >> 16) & 0xFFFFu;
    if (!xecp) return;
    volatile uint32_t *ext = (volatile uint32_t *)(cap + (xecp << 2));
    for (int n = 0; n < 16; n++) {
        uint32_t v = rd(ext);
        uint32_t id = v & 0xFFu;
        uint32_t next = (v >> 8) & 0xFFu;
        if (id == 1) {
            wr(ext, v | (1u << 24));
            for (int i = 0; i < 200; i++) {
                if ((rd(ext) & (1u << 16)) == 0) break;
                wait_ms(10);
            }
            wr(ext + 1, 0x0000FFFFu);
            serial_write("XHCI: ownership\n");
            return;
        }
        if (!next) return;
        ext = (volatile uint32_t *)((uint8_t *)ext + (next << 2));
    }
}

static int halt_and_reset(void) {
    uint32_t cmd = rd(op);
    wr(op, cmd & ~1u);
    for (int i = 0; i < 200; i++) {
        if (rd(op + 1) & 1u) break;
        wait_ms(10);
    }
    wr(op, rd(op) | 2u);
    for (int i = 0; i < 200; i++) {
        if ((rd(op) & 2u) == 0) break;
        wait_ms(10);
    }
    if (rd(op) & 2u) return -1;
    for (int i = 0; i < 200; i++) {
        if ((rd(op + 1) & 0x800u) == 0) return 0;
        wait_ms(10);
    }
    return -2;
}

static int port_reset(int port) {
    volatile uint32_t *psc = op + (0x400 / 4) + port * 4;
    wr(psc, (1u << 9));
    wait_ms(30);
    uint32_t v = rd(psc);
    if ((v & 1u) == 0) return -1;
    wr(psc, (1u << 9) | (1u << 4));
    for (int i = 0; i < 50; i++) {
        wait_ms(10);
        v = rd(psc);
        if ((v & (1u << 4)) == 0 && (v & (1u << 1))) return (int)((v >> 10) & 0xFu);
    }
    return -2;
}

static uint8_t *ctx_at(uint8_t *base, int index) {
    return base + (uint32_t)index * ctx_size;
}

static const uint8_t hid_set1[256] = {
    0, 0, 0, 0,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B,
    0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    0, 0, 0, 0x52 | 0x80, 0x47 | 0x80, 0x49 | 0x80, 0x53 | 0x80,
    0x4F | 0x80, 0x51 | 0x80, 0x4D | 0x80, 0x4B | 0x80, 0x50 | 0x80, 0x48 | 0x80
};

static void post_set1(uint8_t sc, int release) {
    if (!sc) return;
    if (sc & 0x80) input_post_scancode(0xE0);
    input_post_scancode((uint8_t)((sc & 0x7F) | (release ? 0x80 : 0)));
}

static void hid_keyboard(HidEp *ep) {
    static const uint8_t mod_sc[8] = {0x1D, 0x2A, 0x38, 0x5B | 0x80, 0x1D | 0x80, 0x36, 0x38 | 0x80, 0x5C | 0x80};
    uint8_t mods = ep->report[0];
    uint8_t old = ep->prev[0];
    for (int i = 0; i < 8; i++) {
        int now = (mods >> i) & 1;
        int was = (old >> i) & 1;
        if (now == was) continue;
        post_set1(mod_sc[i], !now);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = ep->prev[2 + i];
        if (!k) continue;
        int still = 0;
        for (int j = 0; j < 6; j++) if (ep->report[2 + j] == k) still = 1;
        if (!still) post_set1(hid_set1[k], 1);
    }
    for (int j = 0; j < 6; j++) {
        uint8_t k = ep->report[2 + j];
        if (!k || k == 1) continue;
        int was = 0;
        for (int i = 0; i < 6; i++) if (ep->prev[2 + i] == k) was = 1;
        if (!was) post_set1(hid_set1[k], 0);
    }
    memcpy(ep->prev, ep->report, 8);
}

static int interval_of(int speed, int bint) {
    if (bint < 1) bint = 1;
    if (speed >= 3) {
        int v = bint - 1;
        if (v < 0) v = 0;
        if (v > 15) v = 15;
        return v;
    }
    int v = 3;
    while (bint > 1 && v < 15) { bint >>= 1; v++; }
    return v;
}

static int add_hid(int slot, int speed, int kind, int iface, int epnum, int mps, int bint,
                   uint8_t *input, uint8_t *output) {
    if (ep_count >= 4 || mps < 1 || mps > 16) return -1;
    HidEp *ep = &eps[ep_count];
    memset(ep, 0, sizeof(*ep));
    ep->slot = slot;
    ep->kind = kind;
    ep->iface = iface;
    ep->mps = (uint32_t)mps;
    ep->dci = epnum * 2 + 1;
    if (ring_init(&ep->ring, 16, 1)) return -1;
    uint8_t *sctx = ctx_at(input, 1);
    uint32_t *sd = (uint32_t *)sctx;
    uint32_t entries = (uint32_t)ep->dci;
    if (((sd[0] >> 27) & 0x1F) < entries) sd[0] = (sd[0] & ~(0x1Fu << 27)) | (entries << 27);
    uint32_t *cd = (uint32_t *)ctx_at(input, 0);
    cd[1] |= 1u << ep->dci;
    uint32_t *ed = (uint32_t *)ctx_at(input, ep->dci + 1);
    memset(ed, 0, ctx_size);
    ed[0] = (uint32_t)interval_of(speed, bint) << 16;
    ed[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps << 16);
    ed[2] = (uint32_t)(uintptr_t)ep->ring.trb | 1u;
    ed[4] = (uint32_t)mps | ((uint32_t)mps << 16);
    (void)output;
    (void)iface;
    ep_count++;
    return 0;
}

static void arm_interrupt(HidEp *ep) {
    memset(ep->report, 0, sizeof(ep->report));
    ep->posted = ring_push(&ep->ring, (uint32_t)(uintptr_t)ep->report, 0, ep->mps,
                           (1u << 10) | (1u << 5));
    doorbell(ep->slot, (uint32_t)ep->dci);
}

static void enumerate_hub(Ring *ep0, int hub_slot, int root_port, uint32_t route, int depth,
                           uint32_t slots, int hub_speed, int hub_mtt) {
    uint32_t code = 0;
    uint8_t *hub = halloc(16, 64);
    uint8_t *st = halloc(8, 64);
    if (!hub || !st) return;
    if (control(ep0, hub_slot, 0xA0, 6, 0x2900, 0, hub, 9, 1, &code) || (code != 1 && code != 13)) {
        serial_write("XHCI: hub descriptor failed\n");
        return;
    }
    int ports = hub[2];
    int think = (hub[3] >> 5) & 3;
    if (ports > 8) ports = 8;
    if (ports < 1) return;
    serial_write("XHCI: hub ports ");
    serial_dec((uint32_t)ports);
    serial_write("\n");
    for (int p = 1; p <= ports; p++)
        control(ep0, hub_slot, 0x23, 3, 8, (uint16_t)p, 0, 0, 0, &code);
    wait_ms(100);
    for (int p = 1; p <= ports; p++) {
        memset(st, 0, 4);
        if (control(ep0, hub_slot, 0xA3, 0, 0, (uint16_t)p, st, 4, 1, &code)) continue;
        uint16_t w = (uint16_t)(st[0] | (st[1] << 8));
        if ((w & 1u) == 0) continue;
        control(ep0, hub_slot, 0x23, 3, 4, (uint16_t)p, 0, 0, 0, &code);
        int child_speed = 1;
        for (int t = 0; t < 30; t++) {
            wait_ms(10);
            memset(st, 0, 4);
            if (control(ep0, hub_slot, 0xA3, 0, 0, (uint16_t)p, st, 4, 1, &code)) break;
            w = (uint16_t)(st[0] | (st[1] << 8));
            if (w & (1u << 4)) continue;
            if (w & (1u << 9)) child_speed = 2;
            else if (w & (1u << 10)) child_speed = 3;
            else child_speed = 1;
            break;
        }
        control(ep0, hub_slot, 0x23, 1, 20, (uint16_t)p, 0, 0, 0, &code);
        if ((w & 1u) == 0) continue;
        uint32_t child_route = route | ((uint32_t)p << (depth * 4));
        int parent = 0, parent_port_num = 0;
        if (hub_speed >= 3 && child_speed < 3) {
            parent = hub_slot;
            parent_port_num = p;
        }
        serial_write("XHCI: hub device port ");
        serial_dec((uint32_t)p);
        serial_write("\n");
        enumerate_device(root_port, child_speed, child_route, parent, parent_port_num,
                         slots, depth + 1, hub_mtt, think);
    }
}

static int enumerate_device(int root_port, int speed, uint32_t route, int parent_slot,
                            int parent_port, uint32_t slots, int depth, int hub_mtt, int think) {
    if (depth > 3) return 0;
    uint32_t code = 0, slot = 0;
    Trb *enable = ring_push(&cmd, 0, 0, 0, 9u << 10);
    doorbell(0, 0);
    if (wait_event(33, (uint32_t)(uintptr_t)enable, &code, &slot) || code != 1 || !slot || slot > slots)
        return -1;
    Ring ep0;
    if (ring_init(&ep0, 16, 1)) return -2;
    uint8_t *input = halloc(33u * ctx_size, 64);
    uint8_t *output = halloc(32u * ctx_size, 64);
    if (!input || !output) return -3;
    memset(input, 0, 33u * ctx_size);
    memset(output, 0, 32u * ctx_size);
    dcbaa[slot] = (uint64_t)(uintptr_t)output;
    uint32_t mps = speed <= 2 ? 8 : (speed >= 4 ? 512 : 64);
    uint32_t *cd = (uint32_t *)ctx_at(input, 0);
    cd[1] = (1u << 0) | (1u << 1);
    uint32_t *sd = (uint32_t *)ctx_at(input, 1);
    sd[0] = (route & 0xFFFFFu) | ((uint32_t)speed << 20) | (1u << 27);
    if (parent_slot && hub_mtt) sd[0] |= 1u << 25;
    sd[1] = (uint32_t)(root_port + 1) << 16;
    sd[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8) | ((uint32_t)think << 16);
    uint32_t *ed = (uint32_t *)ctx_at(input, 2);
    ed[1] = (3u << 1) | (4u << 3) | (mps << 16);
    ed[2] = (uint32_t)(uintptr_t)ep0.trb | 1u;
    ed[4] = 8;
    Trb *addr = ring_push(&cmd, (uint32_t)(uintptr_t)input, 0, 0, (11u << 10) | (slot << 24));
    doorbell(0, 0);
    if (wait_event(33, (uint32_t)(uintptr_t)addr, &code, 0) || code != 1) return -4;

    uint8_t *desc = halloc(256, 64);
    if (!desc) return -5;
    memset(desc, 0, 256);
    if (control(&ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 8, 1, &code) || (code != 1 && code != 13)) {
        /* Full-speed devices need 8-byte packets. High-speed devices stall
         * on that, so try the 64-byte size before giving up. */
        ed[1] = (3u << 1) | (4u << 3) | (64u << 16);
        cd[0] = 0;
        cd[1] = 1u << 1;
        Trb *eval = ring_push(&cmd, (uint32_t)(uintptr_t)input, 0, 0, (13u << 10) | (slot << 24));
        doorbell(0, 0);
        wait_event(33, (uint32_t)(uintptr_t)eval, &code, 0);
        if (control(&ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 8, 1, &code) || (code != 1 && code != 13)) {
            serial_write("XHCI: desc code ");
            serial_dec(code);
            serial_write("\n");
            last_err = -5;
            return -5;
        }
        mps = 64;
    }
    uint32_t packet = desc[7];
    if (speed >= 4 && packet == 9) packet = 512;
    if (packet != mps && (packet == 8 || packet == 16 || packet == 32 || packet == 64 || packet == 512)) {
        ed[1] = (3u << 1) | (4u << 3) | (packet << 16);
        cd[0] = 0;
        cd[1] = 1u << 1;
        Trb *eval = ring_push(&cmd, (uint32_t)(uintptr_t)input, 0, 0, (13u << 10) | (slot << 24));
        doorbell(0, 0);
        wait_event(33, (uint32_t)(uintptr_t)eval, &code, 0);
        mps = packet;
    }
    if (control(&ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 18, 1, &code) || (code != 1 && code != 13))
        return -5;
    int dev_class = desc[4];
    int dev_proto = desc[6];
    uint16_t total = 9;
    if (control(&ep0, (int)slot, 0x80, 6, 0x0200, 0, desc, 9, 1, &code) || (code != 1 && code != 13))
        return -6;
    total = (uint16_t)(desc[2] | (desc[3] << 8));
    if (total < 9 || total > 256) total = 256;
    if (control(&ep0, (int)slot, 0x80, 6, 0x0200, 0, desc, total, 1, &code) || (code != 1 && code != 13))
        return -7;
    uint8_t config_val = desc[5];
    int iface = 0, iclass = 0, isub = 0, iproto = 0;
    int is_hub = dev_class == 9;
    int before = ep_count;
    for (int off = 0; off + 2 <= total; ) {
        int dl = desc[off];
        int dt = desc[off + 1];
        if (dl < 2) break;
        if (dt == 4 && dl >= 9) {
            iface = desc[off + 2];
            iclass = desc[off + 5];
            isub = desc[off + 6];
            iproto = desc[off + 7];
            if (iclass == 9) is_hub = 1;
        } else if (dt == 5 && dl >= 7 && iclass == 3) {
            uint8_t addr = desc[off + 2];
            uint8_t attr = desc[off + 3];
            int emp = desc[off + 4] | (desc[off + 5] << 8);
            int bint = desc[off + 6];
            int kind = iproto == 2 ? 2 : 1;
            if ((addr & 0x80) && (attr & 3) == 3)
                add_hid((int)slot, speed, kind, iface, addr & 0x0F, emp, bint, input, output);
        }
        off += dl;
    }
    if (is_hub) {
        saw_hubs++;
        if (control(&ep0, (int)slot, 0x00, 9, config_val, 0, 0, 0, 0, &code) || code != 1)
            return -9;
        enumerate_hub(&ep0, (int)slot, root_port, route, depth, slots, speed, dev_proto == 2);
        note_status();
        return 0;
    }
    if (ep_count == before) return 0;
    memcpy(ctx_at(input, 1), ctx_at(output, 0), ctx_size);
    uint32_t *ns = (uint32_t *)ctx_at(input, 1);
    uint32_t max_dci = (ns[0] >> 27) & 0x1F;
    uint32_t add = 1u;
    for (int i = before; i < ep_count; i++) {
        if ((uint32_t)eps[i].dci > max_dci) max_dci = (uint32_t)eps[i].dci;
        add |= 1u << eps[i].dci;
    }
    ns[0] = (ns[0] & ~(0x1Fu << 27)) | (max_dci << 27);
    cd = (uint32_t *)ctx_at(input, 0);
    cd[0] = 0;
    cd[1] = add;
    Trb *conf = ring_push(&cmd, (uint32_t)(uintptr_t)input, 0, 0, (12u << 10) | (slot << 24));
    doorbell(0, 0);
    if (wait_event(33, (uint32_t)(uintptr_t)conf, &code, 0) || code != 1) return -8;
    if (control(&ep0, (int)slot, 0x00, 9, config_val, 0, 0, 0, 0, &code) || code != 1) return -9;
    for (int i = before; i < ep_count; i++) {
        control(&ep0, (int)slot, 0x21, 0x0B, 0, (uint16_t)eps[i].iface, 0, 0, 0, &code);
        control(&ep0, (int)slot, 0x21, 0x0A, 0, (uint16_t)eps[i].iface, 0, 0, 0, &code);
        arm_interrupt(&eps[i]);
        serial_write(eps[i].kind == 1 ? "XHCI: keyboard\n" : "XHCI: mouse\n");
    }
    note_status();
    return 0;
}

static int start_controller(uint32_t mmio, int bus, int dev, int fn) {
    uint16_t cmdreg = (uint16_t)pci_read(bus, dev, fn, 4);
    pci_write16(bus, dev, fn, 4, (uint16_t)(cmdreg | 0x06));
    heap = XHCI_HEAP;
    heap_used = 0;
    memset(heap, 0, XHCI_HEAP_SIZE);
    cap = (volatile uint8_t *)(uintptr_t)mmio;
    uint8_t caplen = cap[0];
    op = (volatile uint32_t *)(cap + caplen);
    uint32_t hcs1 = rd((volatile uint32_t *)(cap + 4));
    uint32_t hcs2 = rd((volatile uint32_t *)(cap + 8));
    uint32_t hcc = rd((volatile uint32_t *)(cap + 0x10));
    uint32_t dboff = rd((volatile uint32_t *)(cap + 0x14));
    uint32_t rtsoff = rd((volatile uint32_t *)(cap + 0x18));
    db = (volatile uint32_t *)(cap + dboff);
    rt = (volatile uint32_t *)(cap + rtsoff + 0x20);
    ctx_size = (hcc & 4u) ? 64u : 32u;
    uint32_t slots = hcs1 & 0xFFu;
    uint32_t ports = (hcs1 >> 24) & 0xFFu;
    if (!slots || !ports || caplen < 0x10) {
        set_status("USB regs bad");
        return -1;
    }
    saw_controller = 1;
    saw_ports = (int)ports;
    if (slots > 64) slots = 64;
    if (ports > 32) ports = 32;
    serial_write("XHCI: slots ");
    serial_dec(slots);
    serial_write(" ports ");
    serial_dec(ports);
    serial_write("\n");
    bios_handoff();
    if (halt_and_reset()) {
        serial_write("XHCI: reset failed\n");
        set_status("USB reset failed");
        return -2;
    }
    if ((rd(op + 2) & 1u) == 0) {
        serial_write("XHCI: page size\n");
        return -3;
    }
    /* The high and low scratchpad fields are swapped in the register. */
    uint32_t scratch = ((hcs2 >> 16) & 0x3E0u) | ((hcs2 >> 27) & 0x1Fu);
    dcbaa = halloc((slots + 1u) * 8u, 64);
    if (!dcbaa) return -4;
    memset(dcbaa, 0, (slots + 1u) * 8u);
    if (scratch) {
        if (scratch > 32) scratch = 32;
        uint64_t *spa = halloc(scratch * 8u, 64);
        if (!spa) return -5;
        for (uint32_t i = 0; i < scratch; i++) {
            void *page = halloc(4096, 4096);
            if (!page) return -5;
            memset(page, 0, 4096);
            flush_range(page, 4096);
            spa[i] = (uint64_t)(uintptr_t)page;
        }
        dcbaa[0] = (uint64_t)(uintptr_t)spa;
        flush_range(spa, scratch * 8u);
    }
    flush_range(dcbaa, (slots + 1u) * 8u);
    if (ring_init(&cmd, 32, 1) || ring_init(&event, 64, 0)) return -6;
    uint32_t *erst = halloc(16, 64);
    if (!erst) return -7;
    erst[0] = (uint32_t)(uintptr_t)event.trb;
    erst[1] = 0;
    erst[2] = event.size;
    erst[3] = 0;
    wr(op + (0x38 / 4), slots);
    wr64(op + (0x18 / 4), (uint32_t)(uintptr_t)cmd.trb | 1u);
    wr64(op + (0x30 / 4), (uint32_t)(uintptr_t)dcbaa);
    wr(rt + (0x08 / 4), 1);
    wr64(rt + (0x10 / 4), (uint32_t)(uintptr_t)erst);
    wr64(rt + (0x18 / 4), (uint32_t)(uintptr_t)event.trb);
    wr(rt, 0x2);
    wr(op, rd(op) | 1u | 4u);
    for (int i = 0; i < 100; i++) {
        if ((rd(op + 1) & 1u) == 0) break;
        wait_ms(10);
    }
    if (rd(op + 1) & 1u) return -8;
    wait_ms(300);
    ep_count = 0;
    for (uint32_t p = 0; p < ports; p++) {
        int speed = port_reset((int)p);
        if (speed < 1) continue;
        saw_devs++;
        serial_write("XHCI: port ");
        serial_dec(p + 1);
        serial_write(" speed ");
        serial_dec((uint32_t)speed);
        serial_write("\n");
        int rc = enumerate_device((int)p, speed, 0, 0, 0, slots, 0, 0, 0);
        if (rc) {
            last_err = rc;
            serial_write("XHCI: port error ");
            serial_dec((uint32_t)(-rc));
            serial_write("\n");
        }
    }
    ready = ep_count > 0;
    note_status();
    return ready ? 0 : -9;
}

static int same_controller(int bus, int dev, int fn) {
    const BootInfo *bi = bootinfo();
    if (!(bi->magic == BOOTINFO_MAGIC && (bi->xhci_pci & 0x80000000u))) return 0;
    return bus == (int)((bi->xhci_pci >> 16) & 0xFF) &&
           dev == (int)((bi->xhci_pci >> 8) & 0x1F) &&
           fn == (int)(bi->xhci_pci & 7);
}

void xhci_init(void) {
    ready = 0;
    saw_controller = saw_devs = saw_hubs = last_err = 0;
    set_status("No USB chip");
    int tried = 0;
    for (int bus = 0; bus < 256 && !ready; bus++) {
        for (int dev = 0; dev < 32 && !ready; dev++) {
            for (int fn = 0; fn < 8 && !ready; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if (id == 0xFFFFFFFFu || id == 0) {
                    if (fn == 0) break;
                    continue;
                }
                uint32_t cls = pci_read(bus, dev, fn, 8);
                if ((uint8_t)(cls >> 24) != 0x0C || (uint8_t)(cls >> 16) != 0x03 ||
                    (uint8_t)(cls >> 8) != 0x30) {
                    if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
                    continue;
                }
                tried++;
                uint32_t lo = pci_read(bus, dev, fn, 0x10);
                uint32_t hi = ((lo & 7u) == 4u) ? pci_read(bus, dev, fn, 0x14) : 0;
                uint32_t mmio = lo & ~0xFu;
                if (hi) {
                    if (same_controller(bus, dev, fn)) mmio = bootinfo()->xhci_mmio;
                    else {
                        set_status("USB BAR high");
                        if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
                        continue;
                    }
                }
                if (start_controller(mmio, bus, dev, fn) == 0) return;
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    if (!tried) set_status("No USB chip");
    else if (!saw_controller) set_status("USB BAR high");
    serial_write("XHCI: ");
    serial_write(status_line);
    serial_write("\n");
}

void xhci_poll(void) {
    if (!ready) return;
    Trb ev;
    while (event_poll(&ev)) {
        uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
        if (type != 32) continue;
        uint32_t code = ev.d[2] >> 24;
        for (int i = 0; i < ep_count; i++) {
            if ((uint32_t)(uintptr_t)eps[i].posted != ev.d[0]) continue;
            flush_range(eps[i].report, eps[i].mps);
            if (code == 1 || code == 13) {
                if (eps[i].kind == 1) hid_keyboard(&eps[i]);
                else if (eps[i].kind == 2 && eps[i].mps >= 3) {
                    int dx = (int)(int8_t)eps[i].report[1];
                    int dy = (int)(int8_t)eps[i].report[2];
                    input_post_mouse(dx, dy, eps[i].report[0] & 3);
                }
            }
            arm_interrupt(&eps[i]);
        }
    }
}
