#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "input.h"
#include "bootinfo.h"
#include "serial.h"

/* Rings live in uncached memory. The UEFI loader marks this page uncached. */
#define XHCI_HEAP ((uint8_t *)0x1100000)
#define XHCI_HEAP_SIZE (960u * 1024u)
#define MAX_CTL 4
#define MAX_HID 12

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

typedef struct {
    volatile uint8_t *cap;
    volatile uint32_t *op;
    volatile uint32_t *db;
    volatile uint32_t *rt;
    uint32_t ctx_size;
    uint64_t *dcbaa;
    Ring cmd;
    Ring event;
    int alive;
} Ctrl;

static uint8_t *heap;
static uint32_t heap_used;
static Ctrl *ctrls;
static int ctrl_count;

typedef struct {
    int ctrl;
    int slot;
    int dci;
    int kind; /* 1 keyboard, 2 mouse */
    int iface;
    Ring ring;
    uint8_t report[64];
    uint8_t prev[8];
    uint32_t mps;
    Trb *posted;
} HidEp;

static HidEp *eps;
static int ep_count;
static char status_line[48] = "No USB chip";
static int saw_ctl, saw_ports, saw_devs, saw_hubs, last_err, last_code;
static int last_ep_state = -1, last_dq = -1, last_events, last_speed = -1;

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

static char *put_s(char *p, char *end, const char *s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *put_u(char *p, char *end, int v) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    if (v >= 10 && p < end) *p++ = (char)('0' + v / 10);
    if (p < end) *p++ = (char)('0' + (v % 10));
    return p;
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
    char *end = text + sizeof(text) - 1;
    p = put_s(p, end, "USB n");
    p = put_u(p, end, saw_ctl);
    p = put_s(p, end, " p");
    p = put_u(p, end, saw_ports);
    p = put_s(p, end, " d");
    p = put_u(p, end, saw_devs);
    p = put_s(p, end, " h");
    p = put_u(p, end, saw_hubs);
    if (last_err) {
        int e = last_err < 0 ? -last_err : last_err;
        p = put_s(p, end, " e");
        p = put_u(p, end, e);
        p = put_s(p, end, " c");
        p = put_u(p, end, last_code);
        if (last_ep_state >= 0) {
            p = put_s(p, end, " s");
            p = put_u(p, end, last_ep_state);
        }
        if (last_dq >= 0) {
            p = put_s(p, end, " q");
            p = put_u(p, end, last_dq);
        }
        p = put_s(p, end, " v");
        p = put_u(p, end, last_events);
        if (last_speed >= 0) {
            p = put_s(p, end, " z");
            p = put_u(p, end, last_speed);
        }
    }
    *p = 0;
    set_status(text);
}

static int enumerate_device(Ctrl *hc, int root_port, int speed, uint32_t route, int parent_slot,
                            int parent_port, uint32_t slots, int depth, int hub_mtt, int think);
static uint8_t *ctx_at(Ctrl *hc, uint8_t *base, int index);

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

static uint16_t pit_now(void) {
    outb(0x43, 0x00);
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}

/* One PIT period is 10 ms. Count the counter down instead of trusting the
 * tick total, which can run ahead and make a USB timeout return immediately. */
static void wait_ms(int ms) {
    int slices = ms / 10;
    if (slices < 1) slices = 1;
    for (int s = 0; s < slices; s++) {
        uint16_t start = pit_now();
        for (int i = 0; i < 400000; i++) {
            uint16_t now = pit_now();
            if (now > start) break;
            if ((uint16_t)(start - now) > 11000) break;
        }
        input_idle();
    }
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
    flush_range(r->trb, count * sizeof(Trb));
    return 0;
}

static Trb *ring_write(Ring *r, uint32_t a, uint32_t b, uint32_t c, uint32_t flags, int own) {
    uint32_t cyc = own ? r->cycle : (r->cycle ^ 1u);
    Trb *t = &r->trb[r->enqueue];
    t->d[0] = a;
    t->d[1] = b;
    t->d[2] = c;
    t->d[3] = (flags & ~1u) | cyc;
    r->enqueue++;
    if (r->enqueue == r->size) {
        Trb *link = &r->trb[r->size];
        link->d[0] = (uint32_t)(uintptr_t)r->trb;
        link->d[1] = 0;
        link->d[2] = 0;
        link->d[3] = (6u << 10) | (1u << 1) | r->cycle;
        flush_range(link, sizeof(*link));
        r->enqueue = 0;
        r->cycle ^= 1u;
    }
    flush_range(t, sizeof(*t));
    return t;
}

/* Keep a control transfer on one side of the link so its three TDs stay together. */
static void ring_reserve(Ring *r, uint32_t need) {
    if (r->size - r->enqueue >= need) return;
    Trb *link = &r->trb[r->enqueue];
    link->d[0] = (uint32_t)(uintptr_t)r->trb;
    link->d[1] = 0;
    link->d[2] = 0;
    link->d[3] = (6u << 10) | (1u << 1) | r->cycle;
    flush_range(link, sizeof(*link));
    r->enqueue = 0;
    r->cycle ^= 1u;
}

static void erdp_update(Ctrl *hc) {
    uint32_t next = (uint32_t)(uintptr_t)&hc->event.trb[hc->event.dequeue];
    wr64(hc->rt + (0x18 / 4), next | 8u);
}

static int event_poll(Ctrl *hc, Trb *out) {
    Ring *event = &hc->event;
    Trb *t = &event->trb[event->dequeue];
    flush_range(t, sizeof(*t));
    if ((t->d[3] & 1u) != event->cycle) return 0;
    *out = *t;
    event->dequeue++;
    if (event->dequeue == event->size) {
        event->dequeue = 0;
        event->cycle ^= 1u;
    }
    erdp_update(hc);
    return 1;
}

static void flush_range(void *p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~63u;
    uintptr_t end = ((uintptr_t)p + n + 63u) & ~63u;
    for (; a < end; a += 64)
        __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

static void doorbell(Ctrl *hc, int slot, uint32_t target) {
    __asm__ volatile ("mfence" ::: "memory");
    hc->db[slot] = target;
}

static int wait_event(Ctrl *hc, uint32_t type, uint32_t ptr, uint32_t *code, uint32_t *slot, int spins) {
    for (int i = 0; i < spins; i++) {
        Trb ev;
        if (!event_poll(hc, &ev)) {
            wait_ms(5);
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

static void recover_ep0(Ctrl *hc, Ring *ep0, int slot) {
    uint32_t code = 0;
    Trb *stop = ring_write(&hc->cmd, 0, 0, 0, (15u << 10) | (1u << 16) | ((uint32_t)slot << 24), 1);
    doorbell(hc, 0, 0);
    wait_event(hc, 33, (uint32_t)(uintptr_t)stop, &code, 0, 40);
    Trb *rst = ring_write(&hc->cmd, 0, 0, 0, (14u << 10) | (1u << 16) | ((uint32_t)slot << 24), 1);
    doorbell(hc, 0, 0);
    wait_event(hc, 33, (uint32_t)(uintptr_t)rst, &code, 0, 40);
    uint32_t ptr = (uint32_t)(uintptr_t)&ep0->trb[ep0->enqueue] | ep0->cycle;
    Trb *setd = ring_write(&hc->cmd, ptr, 0, 0, (16u << 10) | (1u << 16) | ((uint32_t)slot << 24), 1);
    doorbell(hc, 0, 0);
    wait_event(hc, 33, (uint32_t)(uintptr_t)setd, &code, 0, 40);
}

static int control(Ctrl *hc, Ring *ep0, int slot, uint8_t bm, uint8_t req, uint16_t val,
                   uint16_t idx, void *data, uint16_t len, int dir_in, uint32_t *code) {
    ring_reserve(ep0, 4);
    uint32_t saved = ep0->cycle;
    uint32_t setup_lo = (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t setup_hi = (uint32_t)idx | ((uint32_t)len << 16);
    uint32_t trt = len ? (dir_in ? 3u : 2u) : 0;
    /* Own bit stays off until the data and status TDs are in memory.
     * This chip fetches the next TRB the moment the cycle bit flips. */
    Trb *setup = ring_write(ep0, setup_lo, setup_hi, 8,
                            (2u << 10) | (trt << 16) | (1u << 4) | (1u << 6), 0);
    Trb *data_trb = 0;
    if (len) {
        flush_range(data, len);
        uint32_t dir = dir_in ? (1u << 16) : 0;
        data_trb = ring_write(ep0, (uint32_t)(uintptr_t)data, 0, len, (3u << 10) | dir, 1);
    }
    Trb *status = ring_write(ep0, 0, 0, 0,
                             (4u << 10) | (1u << 5) | ((len && dir_in) ? 0 : (1u << 16)), 1);
    setup->d[3] = (setup->d[3] & ~1u) | saved;
    flush_range(setup, sizeof(*setup));
    doorbell(hc, slot, 1);
    uint32_t setup_ptr = (uint32_t)(uintptr_t)setup & ~0xFu;
    uint32_t status_ptr = (uint32_t)(uintptr_t)status & ~0xFu;
    uint32_t data_ptr = data_trb ? ((uint32_t)(uintptr_t)data_trb & ~0xFu) : 0;
    int events = 0;
    for (int i = 0; i < 100; i++) {
        Trb ev;
        while (event_poll(hc, &ev)) {
            events++;
            uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
            if (type != 32) continue;
            uint32_t got = ev.d[2] >> 24;
            uint32_t ptr = ev.d[0] & ~0xFu;
            uint32_t epid = (ev.d[3] >> 16) & 0x1Fu;
            uint32_t eslot = ev.d[3] >> 24;
            int ptr_hit = ptr == setup_ptr || ptr == status_ptr || (data_ptr && ptr == data_ptr);
            int id_hit = eslot == (uint32_t)slot && epid == 1;
            if (!ptr_hit && !id_hit) continue;
            last_code = (int)got;
            last_events = events;
            if (code) *code = got;
            /* A short data packet is followed by the status event. */
            if (got == 13 && data_ptr && ptr == data_ptr) continue;
            if (got == 1 || got == 13) {
                if (dir_in && data && len) flush_range(data, len);
                return 0;
            }
            recover_ep0(hc, ep0, slot);
            return -1;
        }
        wait_ms(10);
    }
    if (code) *code = 0;
    last_code = 0;
    last_events = events;
    recover_ep0(hc, ep0, slot);
    return -1;
}

/* Address Device leaves the controller's dequeue pointer wherever it finished
 * SET_ADDRESS. Continuing at the start of the ring makes the read time out. */
static void follow_ep0(Ctrl *hc, Ring *ep0, uint8_t *output) {
    uint8_t *ctx = ctx_at(hc, output, 1);
    flush_range(ctx, hc->ctx_size);
    uint32_t *od = (uint32_t *)ctx;
    uint32_t raw = od[2];
    uintptr_t ptr = raw & ~0xFu;
    uintptr_t base = (uintptr_t)ep0->trb;
    uintptr_t end = base + (uintptr_t)ep0->size * sizeof(Trb);
    last_ep_state = (int)(od[0] & 7u);
    if (ptr < base || ptr >= end) {
        last_dq = -1;
        return;
    }
    ep0->enqueue = (uint32_t)((ptr - base) / sizeof(Trb));
    ep0->cycle = raw & 1u;
    last_dq = (int)ep0->enqueue;
}

static void bios_handoff(Ctrl *hc) {
    uint32_t hcc = rd((volatile uint32_t *)(hc->cap + 0x10));
    uint32_t xecp = (hcc >> 16) & 0xFFFFu;
    if (!xecp) return;
    volatile uint32_t *ext = (volatile uint32_t *)(hc->cap + (xecp << 2));
    for (int n = 0; n < 16; n++) {
        uint32_t v = rd(ext);
        uint32_t id = v & 0xFFu;
        uint32_t next = (v >> 8) & 0xFFu;
        if (id == 1) {
            wr(ext, v | (1u << 24));
            for (int i = 0; i < 100; i++) {
                if ((rd(ext) & (1u << 16)) == 0) break;
                wait_ms(10);
            }
            wr(ext + 1, 0x0000FFFFu);
            return;
        }
        if (!next) return;
        ext = (volatile uint32_t *)((uint8_t *)ext + (next << 2));
    }
}

static int halt_and_reset(Ctrl *hc) {
    wr(hc->op, rd(hc->op) & ~1u);
    for (int i = 0; i < 100; i++) {
        if (rd(hc->op + 1) & 1u) break;
        wait_ms(10);
    }
    wr(hc->op, rd(hc->op) | 2u);
    for (int i = 0; i < 100; i++) {
        if ((rd(hc->op) & 2u) == 0) break;
        wait_ms(10);
    }
    if (rd(hc->op) & 2u) return -1;
    for (int i = 0; i < 100; i++) {
        if ((rd(hc->op + 1) & 0x800u) == 0) return 0;
        wait_ms(10);
    }
    return -2;
}

static int port_reset(Ctrl *hc, int port) {
    volatile uint32_t *psc = hc->op + (0x400 / 4) + port * 4;
    uint32_t v = rd(psc);
    if ((v & 1u) == 0) {
        wr(psc, 1u << 9);
        wait_ms(20);
        v = rd(psc);
        if ((v & 1u) == 0) return -1;
    }
    wr(psc, (1u << 9) | (1u << 4));
    int speed = -2;
    for (int i = 0; i < 80; i++) {
        wait_ms(10);
        v = rd(psc);
        if ((v & 1u) == 0) return -1;
        if ((v & (1u << 4)) == 0 && (v & (1u << 1))) {
            speed = (int)((v >> 10) & 0xFu);
            break;
        }
    }
    if (speed < 1 && (rd(psc) & 1u)) {
        wr(psc, (1u << 9) | (1u << 31));
        for (int i = 0; i < 80; i++) {
            wait_ms(10);
            v = rd(psc);
            if ((v & 1u) == 0) return -1;
            if ((v & (1u << 1)) && (v & (1u << 4)) == 0) {
                speed = (int)((v >> 10) & 0xFu);
                break;
            }
        }
    }
    if ((v & 1u) == 0 || (v & (1u << 1)) == 0) return -2;
    if (speed < 1) speed = 3;
    wr(psc, (v & (1u << 9)) | (1u << 17) | (1u << 18) | (1u << 21));
    return speed;
}

static uint8_t *ctx_at(Ctrl *hc, uint8_t *base, int index) {
    return base + (uint32_t)index * hc->ctx_size;
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
        if (!still && k < 0xE8) post_set1(hid_set1[k], 1);
    }
    for (int j = 0; j < 6; j++) {
        uint8_t k = ep->report[2 + j];
        if (!k || k == 1 || k >= 0xE8) continue;
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

static int add_hid(Ctrl *hc, int ctrl_i, int slot, int speed, int kind, int iface, int epnum, int mps, int bint,
                   uint8_t *input) {
    if (ep_count >= MAX_HID || mps < 1 || mps > 64) return -1;
    HidEp *ep = &eps[ep_count];
    memset(ep, 0, sizeof(*ep));
    ep->ctrl = ctrl_i;
    ep->slot = slot;
    ep->kind = kind;
    ep->iface = iface;
    ep->mps = (uint32_t)mps;
    ep->dci = epnum * 2 + 1;
    if (ring_init(&ep->ring, 16, 1)) return -1;
    uint8_t *sctx = ctx_at(hc, input, 1);
    uint32_t *sd = (uint32_t *)sctx;
    uint32_t entries = (uint32_t)ep->dci;
    if (((sd[0] >> 27) & 0x1F) < entries) sd[0] = (sd[0] & ~(0x1Fu << 27)) | (entries << 27);
    uint32_t *cd = (uint32_t *)ctx_at(hc, input, 0);
    cd[1] |= 1u << ep->dci;
    uint32_t *ed = (uint32_t *)ctx_at(hc, input, ep->dci + 1);
    memset(ed, 0, hc->ctx_size);
    ed[0] = (uint32_t)interval_of(speed, bint) << 16;
    ed[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps << 16);
    ed[2] = (uint32_t)(uintptr_t)ep->ring.trb | 1u;
    ed[4] = (uint32_t)mps | ((uint32_t)mps << 16);
    ep_count++;
    return 0;
}

static void arm_interrupt(Ctrl *hc, HidEp *ep) {
    memset(ep->report, 0, sizeof(ep->report));
    ep->posted = ring_write(&ep->ring, (uint32_t)(uintptr_t)ep->report, 0, ep->mps, (1u << 10) | (1u << 5), 1);
    doorbell(hc, ep->slot, (uint32_t)ep->dci);
}

static void disable_slot(Ctrl *hc, uint32_t slot) {
    if (!slot) return;
    uint32_t code = 0;
    Trb *t = ring_write(&hc->cmd, 0, 0, 0, (10u << 10) | (slot << 24), 1);
    doorbell(hc, 0, 0);
    wait_event(hc, 33, (uint32_t)(uintptr_t)t, &code, 0, 40);
}

static int mark_hub(Ctrl *hc, int slot, int ports, uint8_t *input, uint8_t *output) {
    memcpy(ctx_at(hc, input, 1), ctx_at(hc, output, 0), hc->ctx_size);
    uint32_t *sd = (uint32_t *)ctx_at(hc, input, 1);
    uint32_t *cd = (uint32_t *)ctx_at(hc, input, 0);
    sd[0] |= 1u << 26;
    sd[1] = (sd[1] & 0x00FFFFFFu) | ((uint32_t)ports << 24);
    cd[0] = 0;
    cd[1] = 1u;
    flush_range(input, 33u * hc->ctx_size);
    uint32_t code = 0;
    Trb *eval = ring_write(&hc->cmd, (uint32_t)(uintptr_t)input, 0, 0, (13u << 10) | ((uint32_t)slot << 24), 1);
    doorbell(hc, 0, 0);
    if (wait_event(hc, 33, (uint32_t)(uintptr_t)eval, &code, 0, 80) || code != 1) return -1;
    return 0;
}

static int enumerate_hub(Ctrl *hc, Ring *ep0, int hub_slot, int root_port, uint32_t route, int depth,
                          uint32_t slots, int hub_speed, int hub_mtt, uint8_t *input, uint8_t *output) {
    uint32_t code = 0;
    uint8_t *hub = halloc(16, 64);
    uint8_t *st = halloc(8, 64);
    if (!hub || !st) return -1;
    if (control(hc, ep0, hub_slot, 0xA0, 6, 0x2900, 0, hub, 9, 1, &code)) return -1;
    int ports = hub[2];
    int think = (hub[3] >> 5) & 3;
    if (ports > 15) ports = 15;
    if (ports < 1) return -1;
    mark_hub(hc, hub_slot, ports, input, output);
    serial_write("XHCI: hub ports ");
    serial_dec((uint32_t)ports);
    serial_write("\n");
    for (int p = 1; p <= ports; p++)
        control(hc, ep0, hub_slot, 0x23, 3, 8, (uint16_t)p, 0, 0, 0, &code);
    wait_ms(100);
    for (int p = 1; p <= ports; p++) {
        memset(st, 0, 4);
        if (control(hc, ep0, hub_slot, 0xA3, 0, 0, (uint16_t)p, st, 4, 1, &code)) continue;
        uint16_t w = (uint16_t)(st[0] | (st[1] << 8));
        if ((w & 1u) == 0) continue;
        control(hc, ep0, hub_slot, 0x23, 3, 4, (uint16_t)p, 0, 0, 0, &code);
        int child_speed = 1;
        for (int t = 0; t < 20; t++) {
            wait_ms(10);
            memset(st, 0, 4);
            if (control(hc, ep0, hub_slot, 0xA3, 0, 0, (uint16_t)p, st, 4, 1, &code)) break;
            w = (uint16_t)(st[0] | (st[1] << 8));
            if (w & (1u << 4)) continue;
            if (w & (1u << 9)) child_speed = 2;
            else if (w & (1u << 10)) child_speed = 3;
            else child_speed = 1;
            break;
        }
        control(hc, ep0, hub_slot, 0x23, 1, 20, (uint16_t)p, 0, 0, 0, &code);
        if ((w & 1u) == 0) continue;
        uint32_t child_route = route | ((uint32_t)(p & 0xF) << (depth * 4));
        int parent = 0, parent_port_num = 0;
        if (hub_speed >= 3 && child_speed < 3) {
            parent = hub_slot;
            parent_port_num = p;
        }
        serial_write("XHCI: hub device port ");
        serial_dec((uint32_t)p);
        serial_write("\n");
        enumerate_device(hc, root_port, child_speed, child_route, parent, parent_port_num,
                         slots, depth + 1, hub_mtt, think);
    }
    return 0;
}

static int set_ep0_mps(Ctrl *hc, Ring *ep0, int slot, uint8_t *input, uint32_t *ed, uint32_t mps) {
    uint32_t *cd = (uint32_t *)ctx_at(hc, input, 0);
    ed[1] = (3u << 1) | (4u << 3) | (mps << 16);
    ed[2] = (uint32_t)(uintptr_t)&ep0->trb[ep0->enqueue] | ep0->cycle;
    cd[0] = 0;
    cd[1] = 1u << 1;
    flush_range(input, 33u * hc->ctx_size);
    uint32_t code = 0;
    Trb *eval = ring_write(&hc->cmd, (uint32_t)(uintptr_t)input, 0, 0, (13u << 10) | ((uint32_t)slot << 24), 1);
    doorbell(hc, 0, 0);
    wait_event(hc, 33, (uint32_t)(uintptr_t)eval, &code, 0, 80);
    return (int)code;
}

static int enumerate_device(Ctrl *hc, int root_port, int speed, uint32_t route, int parent_slot,
                            int parent_port, uint32_t slots, int depth, int hub_mtt, int think) {
    if (depth > 4) return 0;
    int ctrl_i = (int)(hc - ctrls);
    uint32_t code = 0, slot = 0;
    Trb *enable = ring_write(&hc->cmd, 0, 0, 0, 9u << 10, 1);
    doorbell(hc, 0, 0);
    if (wait_event(hc, 33, (uint32_t)(uintptr_t)enable, &code, &slot, 80) || code != 1 || !slot || slot > slots)
        return -1;
    int rc = 0;
    Ring ep0;
    if (ring_init(&ep0, 16, 1)) { rc = -2; goto fail; }
    uint8_t *input = halloc(33u * hc->ctx_size, 64);
    uint8_t *output = halloc(32u * hc->ctx_size, 64);
    uint8_t *desc = halloc(256, 64);
    if (!input || !output || !desc) { rc = -3; goto fail; }
    memset(input, 0, 33u * hc->ctx_size);
    memset(output, 0, 32u * hc->ctx_size);
    hc->dcbaa[slot] = (uint64_t)(uintptr_t)output;
    flush_range(&hc->dcbaa[slot], 8);
    uint32_t mps = speed <= 2 ? 8u : (speed == 3 ? 64u : 512u);
    uint32_t *cd = (uint32_t *)ctx_at(hc, input, 0);
    cd[1] = (1u << 0) | (1u << 1);
    uint32_t *sd = (uint32_t *)ctx_at(hc, input, 1);
    sd[0] = (route & 0xFFFFFu) | ((uint32_t)(speed & 0xF) << 20) | (1u << 27);
    if (parent_slot && hub_mtt) sd[0] |= 1u << 25;
    sd[1] = (uint32_t)(root_port + 1) << 16;
    sd[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8) | ((uint32_t)think << 16);
    uint32_t *ed = (uint32_t *)ctx_at(hc, input, 2);
    ed[1] = (3u << 1) | (4u << 3) | (mps << 16);
    ed[2] = (uint32_t)(uintptr_t)ep0.trb | 1u;
    ed[4] = 8;
    flush_range(input, 33u * hc->ctx_size);
    Trb *addr = ring_write(&hc->cmd, (uint32_t)(uintptr_t)input, 0, 0, (11u << 10) | (slot << 24), 1);
    doorbell(hc, 0, 0);
    if (wait_event(hc, 33, (uint32_t)(uintptr_t)addr, &code, 0, 80) || code != 1) {
        rc = -4;
        goto fail;
    }
    follow_ep0(hc, &ep0, output);
    /* A transaction error leaves the endpoint halted. The device also needs
     * a moment after SET_ADDRESS before it answers the next request. */
    if (last_ep_state >= 2)
        recover_ep0(hc, &ep0, (int)slot);
    wait_ms(50);
    last_speed = speed;

    memset(desc, 0, 18);
    static const uint32_t try_mps[] = {8, 64, 512};
    int got_desc = 0;
    for (int attempt = 0; attempt < 3 && !got_desc; attempt++) {
        if (attempt) {
            recover_ep0(hc, &ep0, (int)slot);
            wait_ms(20);
        }
        if (!control(hc, &ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 8, 1, &code))
            got_desc = 1;
    }
    if (!got_desc) {
        for (int i = 0; i < 3; i++) {
            if (try_mps[i] == mps) continue;
            set_ep0_mps(hc, &ep0, (int)slot, input, ed, try_mps[i]);
            mps = try_mps[i];
            if (!control(hc, &ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 8, 1, &code)) {
                got_desc = 1;
                break;
            }
        }
    }
    if (!got_desc) {
        serial_write("XHCI: desc code ");
        serial_dec(code);
        serial_write("\n");
        last_err = -5;
        last_code = (int)code;
        rc = -5;
        goto fail;
    }
    uint32_t packet = desc[7];
    if (speed >= 4 && packet == 9) packet = 512;
    if (packet != mps && (packet == 8 || packet == 16 || packet == 32 || packet == 64 || packet == 512)) {
        set_ep0_mps(hc, &ep0, (int)slot, input, ed, packet);
        mps = packet;
    }
    if (control(hc, &ep0, (int)slot, 0x80, 6, 0x0100, 0, desc, 18, 1, &code)) { rc = -5; goto fail; }
    int dev_class = desc[4];
    int dev_proto = desc[6];
    uint16_t total = 9;
    if (control(hc, &ep0, (int)slot, 0x80, 6, 0x0200, 0, desc, 9, 1, &code)) { rc = -6; goto fail; }
    total = (uint16_t)(desc[2] | (desc[3] << 8));
    if (total < 9 || total > 256) total = 256;
    if (control(hc, &ep0, (int)slot, 0x80, 6, 0x0200, 0, desc, total, 1, &code)) { rc = -7; goto fail; }
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
            (void)isub;
        } else if (dt == 5 && dl >= 7 && iclass == 3) {
            uint8_t addr = desc[off + 2];
            uint8_t attr = desc[off + 3];
            int emp = desc[off + 4] | (desc[off + 5] << 8);
            int bint = desc[off + 6];
            int kind = iproto == 2 ? 2 : 1;
            if ((addr & 0x80) && (attr & 3) == 3)
                add_hid(hc, ctrl_i, (int)slot, speed, kind, iface, addr & 0x0F, emp, bint, input);
        }
        off += dl;
    }
    if (is_hub) {
        saw_hubs++;
        if (control(hc, &ep0, (int)slot, 0x00, 9, config_val, 0, 0, 0, 0, &code)) { rc = -9; goto fail; }
        enumerate_hub(hc, &ep0, (int)slot, root_port, route, depth, slots, speed, dev_proto == 2, input, output);
        note_status();
        return 0;
    }
    if (ep_count == before) {
        disable_slot(hc, slot);
        return 0;
    }
    memcpy(ctx_at(hc, input, 1), ctx_at(hc, output, 0), hc->ctx_size);
    uint32_t *ns = (uint32_t *)ctx_at(hc, input, 1);
    uint32_t max_dci = (ns[0] >> 27) & 0x1F;
    uint32_t add = 1u;
    for (int i = before; i < ep_count; i++) {
        if ((uint32_t)eps[i].dci > max_dci) max_dci = (uint32_t)eps[i].dci;
        add |= 1u << eps[i].dci;
    }
    ns[0] = (ns[0] & ~(0x1Fu << 27)) | (max_dci << 27);
    cd = (uint32_t *)ctx_at(hc, input, 0);
    cd[0] = 0;
    cd[1] = add;
    flush_range(input, 33u * hc->ctx_size);
    Trb *conf = ring_write(&hc->cmd, (uint32_t)(uintptr_t)input, 0, 0, (12u << 10) | (slot << 24), 1);
    doorbell(hc, 0, 0);
    if (wait_event(hc, 33, (uint32_t)(uintptr_t)conf, &code, 0, 80) || code != 1) { rc = -8; goto fail; }
    if (control(hc, &ep0, (int)slot, 0x00, 9, config_val, 0, 0, 0, 0, &code)) { rc = -9; goto fail; }
    for (int i = before; i < ep_count; i++) {
        control(hc, &ep0, (int)slot, 0x21, 0x0B, 0, (uint16_t)eps[i].iface, 0, 0, 0, &code);
        control(hc, &ep0, (int)slot, 0x21, 0x0A, 0, (uint16_t)eps[i].iface, 0, 0, 0, &code);
        arm_interrupt(hc, &eps[i]);
        serial_write(eps[i].kind == 1 ? "XHCI: keyboard\n" : "XHCI: mouse\n");
    }
    note_status();
    return 0;
fail:
    if (!last_err) last_err = rc;
    disable_slot(hc, slot);
    return rc;
}

static int pages_ready(void) {
    uint32_t cr0, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (cr0 & 0x80000000u) && (cr3 & ~0xFFFu) == 0x1800000u;
}

static void tlb_flush(void) {
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static void force_uc(uint64_t phys) {
    if (!pages_ready() || phys < 0x2000000ULL || phys >= 0x100000000ULL) return;
    uint64_t base = phys & ~0x1FFFFFULL;
    uint32_t gi = (uint32_t)((base >> 30) & 3ULL);
    uint32_t idx = (uint32_t)((base >> 21) & 0x1FFULL);
    volatile uint64_t *pd = (volatile uint64_t *)(0x1802000u + gi * 4096u);
    pd[idx] = base | 0x83ULL | 0x18ULL;
    tlb_flush();
}

static uint32_t next_window = 0xF2000000u;

static uint32_t map_high_bar(uint64_t phys) {
    if (!pages_ready() || next_window >= 0xF8000000u) return 0;
    uint64_t base = phys & ~0x1FFFFFULL;
    uint32_t off = (uint32_t)(phys - base);
    uint32_t window = next_window;
    next_window += 0x400000u;
    volatile uint64_t *pd = (volatile uint64_t *)0x1805000;
    uint32_t idx = (window >> 21) & 0x1FF;
    pd[idx] = base | 0x83ULL | 0x18ULL;
    pd[idx + 1] = (base + 0x200000ULL) | 0x83ULL | 0x18ULL;
    tlb_flush();
    return window + off;
}

static int same_controller(int bus, int dev, int fn) {
    const BootInfo *bi = bootinfo();
    if (!(bi->magic == BOOTINFO_MAGIC && (bi->xhci_pci & 0x80000000u))) return 0;
    return bus == (int)((bi->xhci_pci >> 16) & 0xFF) &&
           dev == (int)((bi->xhci_pci >> 8) & 0x1F) &&
           fn == (int)(bi->xhci_pci & 7);
}

static int start_controller(uint32_t mmio, int bus, int dev, int fn) {
    if (ctrl_count >= MAX_CTL || !mmio) return -1;
    uint16_t cmdreg = (uint16_t)pci_read(bus, dev, fn, 4);
    pci_write16(bus, dev, fn, 4, (uint16_t)(cmdreg | 0x06));
    Ctrl *hc = &ctrls[ctrl_count];
    memset(hc, 0, sizeof(*hc));
    hc->cap = (volatile uint8_t *)(uintptr_t)mmio;
    uint8_t caplen = hc->cap[0];
    if (caplen < 0x10 || caplen > 0x40) return -1;
    hc->op = (volatile uint32_t *)(hc->cap + caplen);
    uint32_t hcs1 = rd((volatile uint32_t *)(hc->cap + 4));
    uint32_t hcs2 = rd((volatile uint32_t *)(hc->cap + 8));
    uint32_t hcc = rd((volatile uint32_t *)(hc->cap + 0x10));
    uint32_t dboff = rd((volatile uint32_t *)(hc->cap + 0x14));
    uint32_t rtsoff = rd((volatile uint32_t *)(hc->cap + 0x18));
    if (dboff < 0x100 || rtsoff < 0x100) return -1;
    hc->db = (volatile uint32_t *)(hc->cap + dboff);
    hc->rt = (volatile uint32_t *)(hc->cap + rtsoff + 0x20);
    hc->ctx_size = (hcc & 4u) ? 64u : 32u;
    uint32_t slots = hcs1 & 0xFFu;
    uint32_t ports = (hcs1 >> 24) & 0xFFu;
    if (!slots || !ports) return -1;
    if (slots > 64) slots = 64;
    if (ports > 32) ports = 32;
    saw_ctl++;
    saw_ports += (int)ports;
    serial_write("XHCI: mmio ");
    serial_hex32(mmio);
    serial_write(" slots ");
    serial_dec(slots);
    serial_write(" ports ");
    serial_dec(ports);
    serial_write("\n");
    bios_handoff(hc);
    if (halt_and_reset(hc)) {
        serial_write("XHCI: reset failed\n");
        return -2;
    }
    if ((rd(hc->op + 2) & 1u) == 0) return -3;
    uint32_t scratch = ((hcs2 >> 16) & 0x3E0u) | ((hcs2 >> 27) & 0x1Fu);
    hc->dcbaa = halloc((slots + 1u) * 8u, 64);
    if (!hc->dcbaa) return -4;
    memset(hc->dcbaa, 0, (slots + 1u) * 8u);
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
        hc->dcbaa[0] = (uint64_t)(uintptr_t)spa;
        flush_range(spa, scratch * 8u);
    }
    flush_range(hc->dcbaa, (slots + 1u) * 8u);
    if (ring_init(&hc->cmd, 32, 1) || ring_init(&hc->event, 64, 0)) return -6;
    uint32_t *erst = halloc(16, 64);
    if (!erst) return -7;
    erst[0] = (uint32_t)(uintptr_t)hc->event.trb;
    erst[1] = 0;
    erst[2] = hc->event.size;
    erst[3] = 0;
    wr(hc->op + (0x38 / 4), slots);
    wr64(hc->op + (0x18 / 4), (uint32_t)(uintptr_t)hc->cmd.trb | 1u);
    wr64(hc->op + (0x30 / 4), (uint32_t)(uintptr_t)hc->dcbaa);
    wr(hc->rt + (0x08 / 4), 1);
    wr64(hc->rt + (0x10 / 4), (uint32_t)(uintptr_t)erst);
    wr64(hc->rt + (0x18 / 4), (uint32_t)(uintptr_t)hc->event.trb);
    wr(hc->rt, 0x2);
    wr(hc->op, rd(hc->op) | 1u | 4u);
    for (int i = 0; i < 100; i++) {
        if ((rd(hc->op + 1) & 1u) == 0) break;
        wait_ms(10);
    }
    if (rd(hc->op + 1) & 1u) return -8;
    hc->alive = 1;
    ctrl_count++;
    wait_ms(100);
    uint32_t seen = 0;
    for (int pass = 0; pass < 2; pass++) {
        int keyboard = 0;
        for (int i = 0; i < ep_count; i++) if (eps[i].kind == 1) keyboard = 1;
        if (keyboard) break;
        if (pass) wait_ms(200);
        for (uint32_t p = 0; p < ports; p++) {
            if (seen & (1u << p)) continue;
            int speed = port_reset(hc, (int)p);
            if (speed < 1) continue;
            /* A port that has dropped out of U0 still answers addressing,
             * then fails the next transfer with a bus error. */
            {
                volatile uint32_t *psc = hc->op + (0x400 / 4) + (int)p * 4;
                if (((rd(psc) >> 5) & 0xFu) != 0)
                    wr(psc, (1u << 9) | (1u << 16));
            }
            seen |= 1u << p;
            saw_devs++;
            serial_write("XHCI: port ");
            serial_dec(p + 1);
            serial_write(" speed ");
            serial_dec((uint32_t)speed);
            serial_write("\n");
            int rc = enumerate_device(hc, (int)p, speed, 0, 0, 0, slots, 0, 0, 0);
            if (rc) {
                last_err = rc;
                serial_write("XHCI: port error ");
                serial_dec((uint32_t)(rc < 0 ? -rc : rc));
                serial_write("\n");
            }
        }
    }
    note_status();
    return 0;
}

void xhci_init(void) {
    /* Resetting this controller turns off the firmware keyboard. The laptop
     * keys arrive through that firmware path, and our own device reads still
     * fail with a bus error. Leave the controller running. */
    set_status("USB firmware");
    serial_write("XHCI: firmware keyboard\n");
    return;
    ctrl_count = 0;
    ep_count = 0;
    saw_ctl = saw_ports = saw_devs = saw_hubs = last_err = last_code = 0;
    next_window = 0xF2000000u;
    heap = XHCI_HEAP;
    heap_used = 0;
    memset(heap, 0, XHCI_HEAP_SIZE);
    ctrls = halloc(sizeof(Ctrl) * MAX_CTL, 16);
    eps = halloc(sizeof(HidEp) * MAX_HID, 16);
    if (!ctrls || !eps) {
        set_status("USB mem");
        return;
    }
    memset(ctrls, 0, sizeof(Ctrl) * MAX_CTL);
    memset(eps, 0, sizeof(HidEp) * MAX_HID);
    set_status("No USB chip");
    int tried = 0;
    for (int bus = 0; bus < 256 && ctrl_count < MAX_CTL; bus++) {
        for (int dev = 0; dev < 32 && ctrl_count < MAX_CTL; dev++) {
            for (int fn = 0; fn < 8 && ctrl_count < MAX_CTL; fn++) {
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
                if (lo & 1u) {
                    if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
                    continue;
                }
                uint32_t hi = ((lo & 7u) == 4u) ? pci_read(bus, dev, fn, 0x14) : 0;
                uint64_t phys = ((uint64_t)hi << 32) | (lo & ~0xFu);
                uint32_t mmio = 0;
                if (!hi) {
                    force_uc(phys);
                    mmio = (uint32_t)phys;
                } else if (same_controller(bus, dev, fn) && bootinfo()->xhci_mmio) {
                    mmio = bootinfo()->xhci_mmio;
                } else {
                    mmio = map_high_bar(phys);
                }
                if (!mmio) {
                    set_status("USB BAR high");
                    serial_write("XHCI: unmapped BAR\n");
                } else {
                    start_controller(mmio, bus, dev, fn);
                }
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    if (!tried) set_status("No USB chip");
    else if (!saw_ctl && !last_err) set_status("USB BAR high");
    else note_status();
    serial_write("XHCI: ");
    serial_write(status_line);
    serial_write("\n");
}

void xhci_poll(void) {
    if (!ctrls || !eps) return;
    for (int c = 0; c < ctrl_count; c++) {
        Ctrl *hc = &ctrls[c];
        if (!hc->alive) continue;
        Trb ev;
        int guard = 0;
        while (guard++ < 32 && event_poll(hc, &ev)) {
            uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
            if (type != 32) continue;
            uint32_t code = ev.d[2] >> 24;
            for (int i = 0; i < ep_count; i++) {
                if (eps[i].ctrl != c) continue;
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
                arm_interrupt(hc, &eps[i]);
            }
        }
    }
}
