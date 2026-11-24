#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "gfx.h"
#include "input.h"
#include "bootinfo.h"
#include "memmap.h"
#include "serial.h"

/* Rings live in uncached memory. The UEFI loader marks this page uncached. */
#define XHCI_HEAP ((uint8_t *)0x1100000)
#define XHCI_HEAP_SIZE (960u * 1024u)
#define MAX_CTL 4
#define MAX_HID 16
#define USB_MAX 16
#define MAX_IF 8

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
    uint32_t slots;
    uint32_t ports;
    uint32_t usb3;
    uint64_t *dcbaa;
    Ring cmd;
    Ring event;
    int alive;
} Ctrl;

static uint8_t *heap;
static uint32_t heap_used;
static Ctrl *ctrls;
static int ctrl_count;

enum { HID_NONE = 0, HID_KBD = 1, HID_MOUSE = 2, HID_TABLET = 3 };

typedef struct {
    int ctrl;
    int slot;
    int dci;
    int kind;
    int iface;
    int armed;
    int need_reset;
    int resets;
    Ring ring;
    uint8_t report[64];
    uint8_t prev[8];
    uint32_t mps;
    uint32_t repeat_at;
    Trb *posted;
} HidEp;

static HidEp *eps;
static int ep_count;
static char status_line[48] = "No USB chip";
static int saw_ctl, saw_ports, saw_devs, saw_hubs;
static uint32_t kbd_reports, mouse_reports, hid_errors;
static uint8_t hid_last_err;

/* One line per device on DIAG. Completion codes: 0xFF not run, 0 timeout. */
enum { US_FAIL = 0, US_HID, US_SKIP, US_HUB, US_NOHID, US_RESET };
typedef struct {
    uint8_t ctrl, root, hport, speed, slot, depth;
    uint8_t es, ad1, gd8, ad2, gd18, cfg;
    uint8_t scheme, tries, sst, est, state, kinds;
    uint8_t cls, nports, mps;
    uint8_t prev_scheme, prev_ad1, prev_gd8, prev_ad2;
    uint16_t vid, pid;
    uint32_t psc;
} UsbInfo;
static UsbInfo usb_info[USB_MAX];
static UsbInfo usb_spare;
static int usb_info_n;

/* Why the first command (Enable Slot) did or did not run. DIAG prints this. */
enum {
    TR_NONE = 0,
    TR_MAP,
    TR_HALT,
    TR_CNR,
    TR_PAGE,
    TR_SCRATCH,
    TR_RING,
    TR_RUN,
    TR_CRCR,
    TR_NOCCS,
    TR_PORT,
    TR_CMD,
    TR_OK
};
static const char *const tr_name[] = {
    "none", "map", "halt", "cnr", "page", "scratch", "ring", "run", "crcr",
    "noccs", "port", "cmd", "ok"
};
typedef struct {
    uint8_t bus, dev, fn;
    uint8_t stage;
    uint8_t code;
    uint8_t timeout;
    uint8_t have_code;
    uint32_t usbsts;
    uint32_t crcr;
    uint32_t portsc;
    Ctrl *hc;
} CmdTrace;
static CmdTrace traces[4];
static int trace_n;
static CmdTrace *cur_trace;
static uint32_t last_portsc;

static uint32_t rd(volatile uint32_t *r) { return *r; }
static void wr(volatile uint32_t *r, uint32_t v) { *r = v; }
static void wr64(volatile uint32_t *r, uint32_t low) {
    r[0] = low;
    r[1] = 0;
}

static void trace_open(int bus, int dev, int fn, Ctrl *hc) {
    if (trace_n >= 4) {
        cur_trace = 0;
        return;
    }
    cur_trace = &traces[trace_n++];
    memset(cur_trace, 0, sizeof(*cur_trace));
    cur_trace->bus = (uint8_t)bus;
    cur_trace->dev = (uint8_t)dev;
    cur_trace->fn = (uint8_t)fn;
    cur_trace->hc = hc;
    cur_trace->stage = TR_NONE;
}

static void trace_snap(int stage) {
    if (!cur_trace) return;
    if (stage) cur_trace->stage = (uint8_t)stage;
    if (cur_trace->hc && cur_trace->hc->op) {
        cur_trace->usbsts = rd(cur_trace->hc->op + 1);
        cur_trace->crcr = rd(cur_trace->hc->op + (0x18 / 4));
    }
}

/* Record only the first command on this controller. */
static void trace_command(Ctrl *hc, int timed_out, uint32_t code) {
    CmdTrace *t = cur_trace;
    if (!t || t->hc != hc || t->have_code || t->timeout) return;
    t->timeout = timed_out ? 1 : 0;
    t->have_code = timed_out ? 0 : 1;
    t->code = (uint8_t)code;
    t->stage = timed_out ? TR_CMD : (code == 1 ? TR_OK : TR_CMD);
    t->usbsts = rd(hc->op + 1);
    t->crcr = rd(hc->op + (0x18 / 4));
}

const char *xhci_status_line(void) { return status_line; }

static void set_status(const char *text) {
    int i = 0;
    for (; text[i] && i < (int)sizeof(status_line) - 1; i++) status_line[i] = text[i];
    status_line[i] = 0;
}

void xhci_set_status_line(const char *text) { set_status(text); }

void xhci_append_status(const char *extra) {
    int i = 0;
    while (status_line[i] && i < (int)sizeof(status_line) - 1) i++;
    for (int k = 0; extra[k] && i < (int)sizeof(status_line) - 1; k++) status_line[i++] = extra[k];
    status_line[i] = 0;
}

static char *put_s(char *p, char *end, const char *s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *put_u(char *p, char *end, uint32_t v) {
    char rev[10];
    int n = 0;
    if (!v) rev[n++] = '0';
    while (v && n < 10) { rev[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < end) *p++ = rev[--n];
    return p;
}

static char *put_x(char *p, char *end, uint32_t v, int digits) {
    static const char h[] = "0123456789ABCDEF";
    for (int s = (digits - 1) * 4; s >= 0 && p < end; s -= 4) *p++ = h[(v >> s) & 0xF];
    return p;
}

/* Completion code, or t for a timeout, or - when the step never ran. */
static char *put_cc(char *p, char *end, uint8_t code) {
    if (p >= end) return p;
    if (code == 0xFF) { *p++ = '-'; return p; }
    if (code == 0) { *p++ = 't'; return p; }
    return put_u(p, end, code);
}

static void note_status(void) {
    int keyboard = 0, mouse = 0;
    for (int i = 0; i < ep_count; i++) {
        if (eps[i].kind == HID_KBD) keyboard = 1;
        if (eps[i].kind == HID_MOUSE || eps[i].kind == HID_TABLET) mouse = 1;
    }
    char text[48];
    char *p = text;
    char *end = text + sizeof(text) - 1;
    p = put_s(p, end, keyboard ? "USB keyboard ready" : "USB");
    if (mouse) p = put_s(p, end, " +mouse");
    if (!keyboard) {
        p = put_s(p, end, " n");
        p = put_u(p, end, (uint32_t)saw_ctl);
        p = put_s(p, end, " d");
        p = put_u(p, end, (uint32_t)saw_devs);
        p = put_s(p, end, " h");
        p = put_u(p, end, (uint32_t)saw_hubs);
    }
    *p = 0;
    set_status(text);
}

static void *halloc(uint32_t size, uint32_t align) {
    uint32_t a = (heap_used + align - 1u) & ~(align - 1u);
    if (a + size > XHCI_HEAP_SIZE) return 0;
    heap_used = a + size;
    return heap + a;
}

/* Zeroed memory from the USB heap; valid after xhci_init. */
void *xhci_heap_alloc(uint32_t size) { return heap ? halloc(size, 16) : 0; }

static void flush_range(void *p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~63u;
    uintptr_t end = ((uintptr_t)p + n + 63u) & ~63u;
    for (; a < end; a += 64)
        __asm__ volatile ("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

static void wait_ms(int ms) {
    timer_wait_ms(ms);
    input_idle();
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

/* Keep a control transfer on one side of the link so its TRBs stay together. */
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

static void doorbell(Ctrl *hc, int slot, uint32_t target) {
    __asm__ volatile ("mfence" ::: "memory");
    hc->db[slot] = target;
}

static void hid_event(int c, const Trb *ev);

/* Events a waiter does not want still reach the HID code, so reports that
 * arrive while another device enumerates are not lost. */
static void dispatch(Ctrl *hc, const Trb *ev) {
    uint32_t type = (ev->d[3] >> 10) & 0x3Fu;
    if (type == 32) hid_event((int)(hc - ctrls), ev);
}

/* Returns the completion code, or 0 when no completion came back. */
static int command(Ctrl *hc, uint32_t ptr, uint32_t flags, uint32_t *slot_out) {
    Trb *t = ring_write(&hc->cmd, ptr, 0, 0, flags, 1);
    doorbell(hc, 0, 0);
    uint32_t want = (uint32_t)(uintptr_t)t;
    for (int ms = 0; ms < 500; ms++) {
        Trb ev;
        while (event_poll(hc, &ev)) {
            uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
            if (type == 33 && ev.d[0] == want) {
                if (slot_out) *slot_out = ev.d[3] >> 24;
                uint32_t cc = ev.d[2] >> 24;
                return cc ? (int)cc : 0xFE;
            }
            dispatch(hc, &ev);
        }
        timer_wait_ms(1);
    }
    return 0;
}

static void recover_ep(Ctrl *hc, Ring *ring, int slot, int dci) {
    uint32_t sd = ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
    command(hc, 0, (15u << 10) | sd, 0);
    command(hc, 0, (14u << 10) | sd, 0);
    uint32_t ptr = (uint32_t)(uintptr_t)&ring->trb[ring->enqueue] | ring->cycle;
    command(hc, ptr, (16u << 10) | sd, 0);
}

static void trb_cycle(Trb *t, uint32_t cycle) {
    t->d[3] = (t->d[3] & ~1u) | (cycle & 1u);
    flush_range(t, sizeof(*t));
}

/* One control transfer on EP0. Returns 1 on success (short packets count),
 * the completion code on an error, or 0 on a timeout. */
static int control(Ctrl *hc, Ring *ep0, int slot, uint8_t bm, uint8_t req, uint16_t val,
                   uint16_t idx, void *data, uint16_t len) {
    int dir_in = (bm & 0x80) != 0;
    ring_reserve(ep0, 4);
    uint32_t cycle = ep0->cycle;
    uint32_t setup_lo = (uint32_t)bm | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t setup_hi = (uint32_t)idx | ((uint32_t)len << 16);
    uint32_t trt = len ? (dir_in ? 3u : 2u) : 0;
    /* Setup Stage: type 2, IDT, TRT. Bits 1-4 are reserved and stay clear. */
    Trb *setup = ring_write(ep0, setup_lo, setup_hi, 8,
                            (2u << 10) | (trt << 16) | (1u << 6), 0);
    Trb *data_trb = 0;
    if (len) {
        if (dir_in) memset(data, 0, len);
        flush_range(data, len);
        uint32_t dir = dir_in ? (1u << 16) : 0;
        /* ISP so a short IN data stage reports and moves on to Status. */
        data_trb = ring_write(ep0, (uint32_t)(uintptr_t)data, 0, len,
                              (3u << 10) | dir | (dir_in ? (1u << 2) : 0), 0);
    }
    uint32_t status_dir = (len && dir_in) ? 0u : (1u << 16);
    Trb *status = ring_write(ep0, 0, 0, 0, (4u << 10) | (1u << 5) | status_dir, 0);
    if (data_trb) trb_cycle(data_trb, cycle);
    trb_cycle(status, cycle);
    trb_cycle(setup, cycle);
    doorbell(hc, slot, 1);
    uint32_t setup_ptr = (uint32_t)(uintptr_t)setup;
    uint32_t data_ptr = data_trb ? (uint32_t)(uintptr_t)data_trb : 1u;
    uint32_t status_ptr = (uint32_t)(uintptr_t)status;
    for (int ms = 0; ms < 500; ms++) {
        Trb ev;
        while (event_poll(hc, &ev)) {
            uint32_t type = (ev.d[3] >> 10) & 0x3Fu;
            uint32_t epid = (ev.d[3] >> 16) & 0x1Fu;
            uint32_t eslot = ev.d[3] >> 24;
            if (type != 32 || eslot != (uint32_t)slot || epid != 1) {
                dispatch(hc, &ev);
                continue;
            }
            uint32_t got = ev.d[2] >> 24;
            uint32_t ptr = ev.d[0] & ~0xFu;
            if (ptr != setup_ptr && ptr != data_ptr && ptr != status_ptr) continue;
            if (got == 13 && ptr == data_ptr) continue;
            if (got == 1 || got == 13) {
                if (dir_in && len) flush_range(data, len);
                return 1;
            }
            recover_ep(hc, ep0, slot, 1);
            return got ? (int)got : 0xFE;
        }
        timer_wait_ms(1);
    }
    recover_ep(hc, ep0, slot, 1);
    return 0;
}

static int control_retry(Ctrl *hc, Ring *ep0, int slot, uint8_t bm, uint8_t req, uint16_t val,
                         uint16_t idx, void *data, uint16_t len) {
    int cc = 0;
    for (int n = 0; n < 3; n++) {
        cc = control(hc, ep0, slot, bm, req, val, idx, data, len);
        if (cc == 1) return 1;
        if (cc != 0 && cc != 4 && cc != 6) return cc;
        wait_ms(10);
    }
    return cc;
}

static uint8_t *ctx_at(Ctrl *hc, uint8_t *base, int index) {
    return base + (uint32_t)index * hc->ctx_size;
}

/* Continue EP0 where the controller says its dequeue pointer is. */
static void follow_ep0(Ctrl *hc, Ring *ep0, uint8_t *output) {
    uint8_t *ctx = ctx_at(hc, output, 1);
    flush_range(ctx, hc->ctx_size);
    uint32_t raw = ((uint32_t *)ctx)[2];
    uintptr_t ptr = raw & ~0xFu;
    uintptr_t base = (uintptr_t)ep0->trb;
    uintptr_t end = base + (uintptr_t)ep0->size * sizeof(Trb);
    if (ptr < base || ptr >= end) return;
    ep0->enqueue = (uint32_t)((ptr - base) / sizeof(Trb));
    ep0->cycle = raw & 1u;
}

uint32_t xhci_scratchpad_count(uint32_t hcs2) {
    return ((hcs2 >> 16) & 0x3E0u) | ((hcs2 >> 27) & 0x1Fu);
}

/* Linux drivers/usb/host/pci-quirks.c quirk_usb_handoff_xhci:
 * set OS-owned, wait up to 1 s, force-clear BIOS-owned, then clear the
 * SMI enables (bit 0, bit 4, bits 13-15) and the RW1C status bits 29-31.
 * Writing 0x0000FFFF turns those enables on. */
static void legacy_handoff(volatile uint8_t *cap, XhciHandoff *rec) {
    uint32_t hcc = rd((volatile uint32_t *)(cap + 0x10));
    uint32_t xecp = (hcc >> 16) & 0xFFFFu;
    if (!xecp || (xecp << 2) >= 0x8000u) return;
    volatile uint32_t *ext = (volatile uint32_t *)(cap + (xecp << 2));
    for (int n = 0; n < 16; n++) {
        uint32_t v = rd(ext);
        uint32_t id = v & 0xFFu;
        uint32_t next = (v >> 8) & 0xFFu;
        if (id == 1) {
            rec->have_leg = 1;
            rec->leg_before = v;
            rec->ctl_before = rd(ext + 1);
            wr(ext, v | (1u << 24));
            for (int i = 0; i < 100; i++) {
                if ((rd(ext) & (1u << 16)) == 0) break;
                timer_wait_ms(10);
            }
            uint32_t now = rd(ext);
            if (now & (1u << 16))
                wr(ext, (now | (1u << 24)) & ~(1u << 16));
            uint32_t ctl = rd(ext + 1);
            ctl = (ctl & ~0xE011u) | 0xE0000000u;
            wr(ext + 1, ctl);
            rec->leg_after = rd(ext);
            rec->ctl_after = rd(ext + 1);
            return;
        }
        if (!next) return;
        ext = (volatile uint32_t *)((uint8_t *)ext + (next << 2));
    }
}

static void bios_handoff(Ctrl *hc) {
    XhciHandoff ignored;
    memset(&ignored, 0, sizeof(ignored));
    legacy_handoff((volatile uint8_t *)hc->cap, &ignored);
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

/* Bit set = USB3 root port (1-based offset from the protocol cap). */
static uint32_t usb3_port_mask(volatile uint8_t *cap) {
    uint32_t hcc = rd((volatile uint32_t *)(cap + 0x10));
    uint32_t xecp = (hcc >> 16) & 0xFFFFu;
    if (!xecp || (xecp << 2) >= 0x8000u) return 0;
    volatile uint32_t *ext = (volatile uint32_t *)(cap + (xecp << 2));
    uint32_t mask = 0;
    for (int n = 0; n < 16; n++) {
        uint32_t v = rd(ext);
        uint32_t id = v & 0xFFu;
        uint32_t next = (v >> 8) & 0xFFu;
        if (id == 2) {
            uint32_t major = (v >> 24) & 0xFFu;
            uint32_t info = rd(ext + 2);
            uint32_t off = info & 0xFFu;
            uint32_t cnt = (info >> 8) & 0xFFu;
            if (major >= 3 && off >= 1) {
                for (uint32_t i = 0; i < cnt && off + i <= 32; i++)
                    mask |= 1u << (off - 1u + i);
            }
        }
        if (!next) break;
        ext = (volatile uint32_t *)((uint8_t *)ext + (next << 2));
    }
    return mask;
}

/* Always bus-reset, even a port firmware left enabled: until the reset the
 * device still answers at the address firmware gave it, not address 0.
 * A connect blip during reset is normal. Warm reset is only for a USB3
 * port that Port Reset did not enable. Returns the PORTSC speed ID. */
static int port_reset(Ctrl *hc, int port, int usb3) {
    volatile uint32_t *psc = hc->op + (0x400 / 4) + port * 4;
    uint32_t v = rd(psc);
    if ((v & (1u << 9)) == 0) {
        wr(psc, 1u << 9);
        wait_ms(20);
        v = rd(psc);
    }
    last_portsc = v;
    if ((v & 1u) == 0) return -1;
    wr(psc, (1u << 9) | (1u << 4));
    int speed = -2;
    for (int i = 0; i < 50; i++) {
        wait_ms(10);
        v = rd(psc);
        int pr_clear = (v & (1u << 4)) == 0;
        int prc = (v & (1u << 21)) != 0;
        int ped = (v & (1u << 1)) != 0;
        if (pr_clear && ped && (prc || i > 2)) {
            speed = (int)((v >> 10) & 0xFu);
            if (speed >= 1) break;
        }
    }
    if (speed < 1 && usb3 && (rd(psc) & 1u)) {
        wr(psc, (1u << 9) | (1u << 31));
        for (int i = 0; i < 50; i++) {
            wait_ms(10);
            v = rd(psc);
            if ((v & (1u << 1)) && ((v >> 10) & 0xFu) >= 1) {
                speed = (int)((v >> 10) & 0xFu);
                break;
            }
        }
    }
    last_portsc = v;
    if (speed < 1 || (v & (1u << 1)) == 0) return -2;
    wr(psc, (1u << 9) | (1u << 17) | (1u << 18) | (1u << 21) | (1u << 22) |
            (usb3 ? (1u << 19) : 0));
    v = rd(psc);
    if (((v >> 5) & 0xFu) != 0 && ((v >> 5) & 0xFu) != 3) {
        /* LWS (bit 16) plus PLS=0 asks the link to enter U0. */
        wr(psc, (1u << 9) | (1u << 16));
        for (int i = 0; i < 10; i++) {
            wait_ms(1);
            v = rd(psc);
            if (((v >> 5) & 0xFu) == 0) break;
        }
    }
    /* TRSTRCY is 10 ms. */
    wait_ms(20);
    last_portsc = rd(psc);
    return speed;
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
    0x4F | 0x80, 0x51 | 0x80, 0x4D | 0x80, 0x4B | 0x80, 0x50 | 0x80, 0x48 | 0x80,
    /* 0x53 Num Lock, then keypad / * - + Enter 1-9 0 . */
    0x45, 0x35 | 0x80, 0x37, 0x4A, 0x4E, 0x1C | 0x80,
    0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53
};

static void post_set1(uint8_t sc, int release) {
    if (!sc) return;
    input_post_set1((uint8_t)(sc & 0x7F), (sc & 0x80) != 0, release);
}

static void hid_keyboard(HidEp *ep) {
    static const uint8_t mod_sc[8] = {0x1D, 0x2A, 0x38, 0x5B | 0x80, 0x1D | 0x80, 0x36, 0x38 | 0x80, 0x5C | 0x80};
    /* All 0x01 is the rollover error report. Keep the previous state. */
    if (ep->report[2] == 1 && ep->report[3] == 1) return;
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
        if (k < 4) continue;
        int still = 0;
        for (int j = 0; j < 6; j++) if (ep->report[2 + j] == k) still = 1;
        if (!still && k < 0xE0) post_set1(hid_set1[k], 1);
    }
    for (int j = 0; j < 6; j++) {
        uint8_t k = ep->report[2 + j];
        if (k < 4 || k >= 0xE0) continue;
        int was = 0;
        for (int i = 0; i < 6; i++) if (ep->prev[2 + i] == k) was = 1;
        if (!was) post_set1(hid_set1[k], 0);
    }
    memcpy(ep->prev, ep->report, 8);
}

static void arm_interrupt(Ctrl *hc, HidEp *ep) {
    memset(ep->report, 0, sizeof(ep->report));
    flush_range(ep->report, sizeof(ep->report));
    ep->posted = ring_write(&ep->ring, (uint32_t)(uintptr_t)ep->report, 0, ep->mps,
                            (1u << 10) | (1u << 5) | (1u << 2), 1);
    ep->armed = 1;
    doorbell(hc, ep->slot, (uint32_t)ep->dci);
}

static void hid_event(int c, const Trb *ev) {
    if (!eps) return;
    uint32_t slot = ev->d[3] >> 24;
    uint32_t dci = (ev->d[3] >> 16) & 0x1Fu;
    for (int i = 0; i < ep_count; i++) {
        HidEp *ep = &eps[i];
        if (ep->ctrl != c || (uint32_t)ep->slot != slot || (uint32_t)ep->dci != dci) continue;
        if ((uint32_t)(uintptr_t)ep->posted != (ev->d[0] & ~0xFu)) return;
        uint32_t code = ev->d[2] >> 24;
        Ctrl *hc = &ctrls[c];
        if (code != 1 && code != 13) {
            hid_errors++;
            hid_last_err = (uint8_t)code;
            ep->armed = 0;
            ep->need_reset = 1;
            return;
        }
        flush_range(ep->report, ep->mps);
        if (ep->kind == HID_KBD) {
            kbd_reports++;
            hid_keyboard(ep);
        } else if (ep->kind == HID_MOUSE) {
            mouse_reports++;
            int dx = (int)(int8_t)ep->report[1];
            int dy = (int)(int8_t)ep->report[2];
            input_post_mouse(dx, dy, ep->report[0] & 7);
        } else if (ep->kind == HID_TABLET && ep->mps >= 5) {
            mouse_reports++;
            int x = ep->report[1] | (ep->report[2] << 8);
            int y = ep->report[3] | (ep->report[4] << 8);
            int w = gfx_width() ? gfx_width() : 1;
            int h = gfx_height() ? gfx_height() : 1;
            input_set_pointer(x * w / 32768, y * h / 32768, ep->report[0] & 7);
        }
        arm_interrupt(hc, ep);
        return;
    }
}

static void disable_slot(Ctrl *hc, uint32_t slot) {
    if (!slot) return;
    command(hc, 0, (10u << 10) | (slot << 24), 0);
    hc->dcbaa[slot] = 0;
    flush_range(&hc->dcbaa[slot], 8);
}

/* Speed ID 1 is full, 2 is low, 3 is high, 4 and above are SuperSpeed. */
static uint32_t default_mps(int speed) {
    if (speed == 2) return 8;
    if (speed >= 4) return 512;
    return 64;
}

/* xHCI endpoint Interval is 2^n x 125 us. HS and SS give the exponent + 1
 * in bInterval; FS and LS give frames, as Linux xhci_parse_frame_interval. */
static uint32_t interval_of(int speed, int bint) {
    if (bint < 1) bint = 1;
    if (speed >= 3) {
        int v = bint - 1;
        if (v > 15) v = 15;
        return (uint32_t)v;
    }
    int v = 0;
    int f = bint * 8;
    while ((f >> 1) > 0) { f >>= 1; v++; }
    if (v < 3) v = 3;
    if (v > 10) v = 10;
    return (uint32_t)v;
}

typedef struct {
    Ring *ep0;
    int slot;
    int port;
    int ss;
    uint8_t *st;
} HubPos;

typedef struct {
    int root;
    int speed;
    uint32_t route;
    int depth;
    int tt_slot;
    int tt_port;
    HubPos *hub;
} DevPos;

typedef struct {
    int slot;
    Ring ep0;
    uint8_t *in;
    uint8_t *out;
    uint8_t *buf;
    uint32_t mps;
} Dev;

typedef struct {
    uint8_t num, cls, sub, proto;
    uint8_t ep, interval;
    uint16_t mps;
    uint16_t rlen;
} IfDesc;

static UsbInfo *info_new(Ctrl *hc, DevPos *pos) {
    UsbInfo *ui = usb_info_n < USB_MAX ? &usb_info[usb_info_n++] : &usb_spare;
    memset(ui, 0, sizeof(*ui));
    ui->ctrl = (uint8_t)(hc - ctrls);
    ui->root = (uint8_t)(pos->root + 1);
    ui->hport = pos->hub ? (uint8_t)pos->hub->port : 0;
    ui->speed = (uint8_t)pos->speed;
    ui->depth = (uint8_t)pos->depth;
    ui->es = ui->ad1 = ui->gd8 = ui->ad2 = ui->gd18 = ui->cfg = 0xFF;
    ui->prev_ad1 = ui->prev_gd8 = ui->prev_ad2 = 0xFF;
    ui->psc = last_portsc;
    return ui;
}

static void ctx_states(Ctrl *hc, Dev *d, UsbInfo *ui) {
    flush_range(d->out, hc->ctx_size * 2u);
    uint32_t *slot_ctx = (uint32_t *)ctx_at(hc, d->out, 0);
    uint32_t *ep0_ctx = (uint32_t *)ctx_at(hc, d->out, 1);
    ui->sst = (uint8_t)((slot_ctx[3] >> 27) & 0x1Fu);
    ui->est = (uint8_t)(ep0_ctx[0] & 7u);
}

static void fix_mps(Ctrl *hc, Dev *d, int speed) {
    uint32_t packet = d->buf[7];
    if (speed >= 4) packet = 512;
    if (packet != 8 && packet != 16 && packet != 32 && packet != 64 && packet != 512) return;
    if (packet == d->mps) return;
    uint32_t *cd = (uint32_t *)ctx_at(hc, d->in, 0);
    uint32_t *ed = (uint32_t *)ctx_at(hc, d->in, 2);
    ed[1] = (ed[1] & 0xFFFFu) | (packet << 16);
    cd[0] = 0;
    cd[1] = 1u << 1;
    flush_range(d->in, 33u * hc->ctx_size);
    command(hc, (uint32_t)(uintptr_t)d->in, (13u << 10) | ((uint32_t)d->slot << 24), 0);
    cd[1] = (1u << 0) | (1u << 1);
    flush_range(d->in, 33u * hc->ctx_size);
    d->mps = packet;
}

/* Scheme 1 is Linux's: Address Device BSR=1, read 8 bytes at address 0,
 * then Address Device BSR=0. Scheme 2 sends SET_ADDRESS first. */
static int dev_address(Ctrl *hc, Dev *d, DevPos *pos, UsbInfo *ui, int scheme) {
    uint32_t slot = 0;
    int cc = command(hc, 0, 9u << 10, &slot);
    trace_command(hc, cc == 0, (uint32_t)cc);
    ui->es = (uint8_t)cc;
    if (cc != 1 || !slot || slot > hc->slots) return -1;
    d->slot = (int)slot;
    ui->slot = (uint8_t)slot;
    if (ring_init(&d->ep0, 32, 1)) return -2;
    memset(d->in, 0, 33u * hc->ctx_size);
    memset(d->out, 0, 32u * hc->ctx_size);
    flush_range(d->out, 32u * hc->ctx_size);
    hc->dcbaa[slot] = (uint64_t)(uintptr_t)d->out;
    flush_range(&hc->dcbaa[slot], 8);
    d->mps = default_mps(pos->speed);
    uint32_t *cd = (uint32_t *)ctx_at(hc, d->in, 0);
    uint32_t *sd = (uint32_t *)ctx_at(hc, d->in, 1);
    uint32_t *ed = (uint32_t *)ctx_at(hc, d->in, 2);
    cd[1] = (1u << 0) | (1u << 1);
    sd[0] = (pos->route & 0xFFFFFu) | ((uint32_t)(pos->speed & 0xF) << 20) | (1u << 27);
    sd[1] = (uint32_t)(pos->root + 1) << 16;
    sd[2] = (uint32_t)pos->tt_slot | ((uint32_t)pos->tt_port << 8);
    ed[1] = (3u << 1) | (4u << 3) | (d->mps << 16);
    ed[2] = (uint32_t)(uintptr_t)d->ep0.trb | 1u;
    ed[3] = 0;
    ed[4] = 8;
    flush_range(d->in, 33u * hc->ctx_size);
    uint32_t in = (uint32_t)(uintptr_t)d->in;
    if (scheme == 1) {
        cc = command(hc, in, (11u << 10) | (1u << 9) | (slot << 24), 0);
        ui->ad1 = (uint8_t)cc;
        if (cc != 1) return -3;
        follow_ep0(hc, &d->ep0, d->out);
        cc = control_retry(hc, &d->ep0, (int)slot, 0x80, 6, 0x0100, 0, d->buf, 8);
        ui->gd8 = (uint8_t)cc;
        if (cc != 1) return -4;
        fix_mps(hc, d, pos->speed);
        ed[2] = (uint32_t)(uintptr_t)&d->ep0.trb[d->ep0.enqueue] | d->ep0.cycle;
        flush_range(d->in, 33u * hc->ctx_size);
        cc = command(hc, in, (11u << 10) | (slot << 24), 0);
        ui->ad2 = (uint8_t)cc;
        if (cc != 1) return -5;
        wait_ms(5);
    } else {
        cc = command(hc, in, (11u << 10) | (slot << 24), 0);
        ui->ad2 = (uint8_t)cc;
        if (cc != 1) return -5;
        wait_ms(5);
        follow_ep0(hc, &d->ep0, d->out);
        cc = control_retry(hc, &d->ep0, (int)slot, 0x80, 6, 0x0100, 0, d->buf, 8);
        ui->gd8 = (uint8_t)cc;
        if (cc != 1) return -4;
        fix_mps(hc, d, pos->speed);
    }
    follow_ep0(hc, &d->ep0, d->out);
    cc = control_retry(hc, &d->ep0, (int)slot, 0x80, 6, 0x0100, 0, d->buf, 18);
    ui->gd18 = (uint8_t)cc;
    if (cc != 1) return -6;
    return 0;
}

static int parse_config(const uint8_t *buf, int total, IfDesc *ifs, int *n_if) {
    int n = 0;
    int cur = -1;
    for (int off = 0; off + 2 <= total; ) {
        int dl = buf[off];
        int dt = buf[off + 1];
        if (dl < 2 || off + dl > total) break;
        if (dt == 4 && dl >= 9) {
            cur = -1;
            if (buf[off + 3] == 0 && n < MAX_IF) {
                cur = n++;
                memset(&ifs[cur], 0, sizeof(ifs[cur]));
                ifs[cur].num = buf[off + 2];
                ifs[cur].cls = buf[off + 5];
                ifs[cur].sub = buf[off + 6];
                ifs[cur].proto = buf[off + 7];
            }
        } else if (dt == 0x21 && dl >= 9 && cur >= 0) {
            for (int k = 0; k < buf[off + 5] && 6 + k * 3 + 2 < dl; k++) {
                if (buf[off + 6 + k * 3] == 0x22) {
                    ifs[cur].rlen = (uint16_t)(buf[off + 7 + k * 3] | (buf[off + 8 + k * 3] << 8));
                    break;
                }
            }
        } else if (dt == 5 && dl >= 7 && cur >= 0 && !ifs[cur].ep) {
            uint8_t addr = buf[off + 2];
            uint8_t attr = buf[off + 3];
            if ((addr & 0x80) && (attr & 3) == 3) {
                ifs[cur].ep = addr;
                ifs[cur].mps = (uint16_t)(buf[off + 4] | (buf[off + 5] << 8));
                ifs[cur].interval = buf[off + 6];
            }
        }
        off += dl;
    }
    *n_if = n;
    return n;
}

/* Interrupt IN endpoint context at DCI 2*ep+1. */
static void ep_ctx(Ctrl *hc, Dev *d, int speed, IfDesc *f, Ring *ring) {
    int dci = (f->ep & 0x0F) * 2 + 1;
    uint32_t *ed = (uint32_t *)ctx_at(hc, d->in, dci + 1);
    memset(ed, 0, hc->ctx_size);
    uint32_t mps = f->mps & 0x7FFu;
    uint32_t mult = speed == 3 ? ((f->mps >> 11) & 3u) : 0;
    uint32_t esit = mps * (mult + 1u);
    ed[0] = interval_of(speed, f->interval) << 16;
    ed[1] = (3u << 1) | (7u << 3) | (mult << 8) | (mps << 16);
    ed[2] = (uint32_t)(uintptr_t)ring->trb | 1u;
    ed[3] = 0;
    ed[4] = (esit & 0xFFFFu) | ((esit & 0xFFFFu) << 16);
}

/* Configure Endpoint with A0 set: copies the slot from the output context,
 * so hub fields and Context Entries take effect (Evaluate Context ignores them). */
static int configure(Ctrl *hc, Dev *d, uint32_t add, int max_dci, int hub, int nports, int ttt) {
    uint32_t *cd = (uint32_t *)ctx_at(hc, d->in, 0);
    uint32_t *sd = (uint32_t *)ctx_at(hc, d->in, 1);
    flush_range(d->out, hc->ctx_size);
    memcpy(sd, ctx_at(hc, d->out, 0), hc->ctx_size);
    uint32_t entries = (sd[0] >> 27) & 0x1Fu;
    if ((uint32_t)max_dci > entries) entries = (uint32_t)max_dci;
    sd[0] = (sd[0] & ~((0x1Fu << 27) | (1u << 26) | (1u << 25))) | (entries << 27);
    if (hub) {
        sd[0] |= 1u << 26;
        sd[1] = (sd[1] & 0x00FFFFFFu) | ((uint32_t)nports << 24);
        sd[2] = (sd[2] & ~(3u << 16)) | ((uint32_t)(ttt & 3) << 16);
    }
    sd[3] = 0;
    cd[0] = 0;
    cd[1] = add | 1u;
    flush_range(d->in, 33u * hc->ctx_size);
    return command(hc, (uint32_t)(uintptr_t)d->in, (12u << 10) | ((uint32_t)d->slot << 24), 0);
}

/* Absolute pointer, as in QEMU's usb-tablet: Generic Desktop Mouse with
 * a 16-bit 0..0x7FFF axis. Hotkey and media interfaces fail this test. */
static int looks_tablet(const uint8_t *r, int n) {
    if (n < 4 || r[0] != 0x05 || r[1] != 0x01 || r[2] != 0x09 || r[3] != 0x02) return 0;
    for (int i = 0; i + 2 < n; i++)
        if (r[i] == 0x26 && r[i + 1] == 0xFF && r[i + 2] == 0x7F) return 1;
    return 0;
}

static void enumerate_device(Ctrl *hc, DevPos *pos);

static int hub_status(Ctrl *hc, HubPos *hp, uint16_t *st, uint16_t *chg) {
    int cc = control(hc, hp->ep0, hp->slot, 0xA3, 0, 0, (uint16_t)hp->port, hp->st, 4);
    if (cc != 1) return -1;
    *st = (uint16_t)(hp->st[0] | (hp->st[1] << 8));
    *chg = (uint16_t)(hp->st[2] | (hp->st[3] << 8));
    return 0;
}

/* SET_FEATURE(PORT_RESET), wait for C_PORT_RESET, clear it, then the
 * 10 ms reset recovery. Returns the xHCI speed ID of the child. */
static int hub_port_reset(Ctrl *hc, HubPos *hp) {
    uint16_t st = 0, chg = 0;
    control(hc, hp->ep0, hp->slot, 0x23, 3, 4, (uint16_t)hp->port, 0, 0);
    int speed = -1;
    for (int t = 0; t < 50; t++) {
        wait_ms(10);
        if (hub_status(hc, hp, &st, &chg)) break;
        if ((st & 1u) == 0) break;
        if ((chg & (1u << 4)) || ((st & (1u << 4)) == 0 && (st & 2u))) {
            if (st & 2u) {
                if (hp->ss) speed = 4;
                else if (st & (1u << 9)) speed = 2;
                else if (st & (1u << 10)) speed = 3;
                else speed = 1;
            }
            break;
        }
    }
    control(hc, hp->ep0, hp->slot, 0x23, 1, 20, (uint16_t)hp->port, 0, 0);
    wait_ms(20);
    last_portsc = ((uint32_t)chg << 16) | st;
    return speed;
}

static void enumerate_hub(Ctrl *hc, Dev *d, DevPos *pos, UsbInfo *ui, IfDesc *f, int cfg_val) {
    int ss = pos->speed >= 4;
    uint8_t *hd = d->buf;
    control(hc, &d->ep0, d->slot, 0x00, 9, (uint16_t)cfg_val, 0, 0, 0);
    int cc = control(hc, &d->ep0, d->slot, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, hd, ss ? 12 : 9);
    if (cc != 1) {
        ui->cfg = (uint8_t)cc;
        return;
    }
    int ports = hd[2];
    int ttt = (hd[3] >> 5) & 3;
    int pwr = hd[5] * 2;
    if (ports > 15) ports = 15;
    if (ports < 1) return;
    ui->nports = (uint8_t)ports;
    Ring *hring = halloc(sizeof(Ring), 16);
    uint32_t add = 0;
    int max_dci = 1;
    if (f && f->ep && hring && !ring_init(hring, 8, 1)) {
        int dci = (f->ep & 0x0F) * 2 + 1;
        ep_ctx(hc, d, pos->speed, f, hring);
        add = 1u << dci;
        max_dci = dci;
    }
    cc = configure(hc, d, add, max_dci, 1, ports, pos->speed == 3 ? ttt : 0);
    ui->cfg = (uint8_t)cc;
    if (ss) control(hc, &d->ep0, d->slot, 0x20, 12, (uint16_t)pos->depth, 0, 0, 0);
    for (int p = 1; p <= ports; p++)
        control(hc, &d->ep0, d->slot, 0x23, 3, 8, (uint16_t)p, 0, 0);
    wait_ms(pwr > 100 ? pwr + 50 : 150);
    uint8_t *st = halloc(8, 64);
    if (!st) return;
    for (int p = 1; p <= ports; p++) {
        HubPos hp;
        hp.ep0 = &d->ep0;
        hp.slot = d->slot;
        hp.port = p;
        hp.ss = ss;
        hp.st = st;
        uint16_t s = 0, chg = 0;
        if (hub_status(hc, &hp, &s, &chg)) continue;
        if ((s & 1u) == 0) continue;
        control(hc, &d->ep0, d->slot, 0x23, 1, 16, (uint16_t)p, 0, 0);
        int speed = hub_port_reset(hc, &hp);
        DevPos child;
        child.root = pos->root;
        child.speed = speed;
        child.route = pos->route | ((uint32_t)(p & 0xF) << (pos->depth * 4));
        child.depth = pos->depth + 1;
        child.tt_slot = pos->tt_slot;
        child.tt_port = pos->tt_port;
        child.hub = &hp;
        if (pos->speed == 3 && speed >= 1 && speed < 3) {
            child.tt_slot = d->slot;
            child.tt_port = p;
        }
        if (speed < 1) {
            UsbInfo *bad = info_new(hc, &child);
            bad->state = US_RESET;
            continue;
        }
        saw_devs++;
        enumerate_device(hc, &child);
    }
}

static void enumerate_device(Ctrl *hc, DevPos *pos) {
    if (pos->depth > 4) return;
    int ctrl_i = (int)(hc - ctrls);
    UsbInfo *ui = info_new(hc, pos);
    Dev d;
    memset(&d, 0, sizeof(d));
    d.in = halloc(33u * hc->ctx_size, 64);
    d.out = halloc(32u * hc->ctx_size, 64);
    d.buf = halloc(512, 64);
    if (!d.in || !d.out || !d.buf) return;
    int first = pos->speed >= 4 ? 2 : 1;
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        int scheme = attempt ? 3 - first : first;
        if (attempt) {
            ui->prev_scheme = ui->scheme;
            ui->prev_ad1 = ui->ad1;
            ui->prev_gd8 = ui->gd8;
            ui->prev_ad2 = ui->ad2;
            ui->es = ui->ad1 = ui->gd8 = ui->ad2 = ui->gd18 = 0xFF;
            int sp = pos->hub ? hub_port_reset(hc, pos->hub)
                              : port_reset(hc, pos->root, (int)((hc->usb3 >> pos->root) & 1u));
            if (sp < 1) break;
            pos->speed = sp;
            ui->speed = (uint8_t)sp;
            ui->psc = last_portsc;
        }
        ui->scheme = (uint8_t)scheme;
        ui->tries = (uint8_t)(attempt + 1);
        d.slot = 0;
        if (dev_address(hc, &d, pos, ui, scheme) == 0) {
            ok = 1;
            break;
        }
        if (d.slot) {
            ctx_states(hc, &d, ui);
            disable_slot(hc, (uint32_t)d.slot);
        }
    }
    if (!ok) {
        ui->state = US_FAIL;
        return;
    }
    ctx_states(hc, &d, ui);
    ui->vid = (uint16_t)(d.buf[8] | (d.buf[9] << 8));
    ui->pid = (uint16_t)(d.buf[10] | (d.buf[11] << 8));
    ui->cls = d.buf[4];
    ui->mps = (uint8_t)(d.mps > 255 ? 255 : d.mps);
    int dev_class = d.buf[4];
    serial_write("XHCI: dev ");
    serial_hex32(((uint32_t)ui->vid << 16) | ui->pid);
    serial_write("\n");
    int cc = control(hc, &d.ep0, d.slot, 0x80, 6, 0x0200, 0, d.buf, 9);
    ui->cfg = (uint8_t)cc;
    if (cc != 1) {
        ui->state = US_FAIL;
        return;
    }
    int total = d.buf[2] | (d.buf[3] << 8);
    if (total < 9) total = 9;
    if (total > 512) total = 512;
    cc = control(hc, &d.ep0, d.slot, 0x80, 6, 0x0200, 0, d.buf, (uint16_t)total);
    ui->cfg = (uint8_t)cc;
    if (cc != 1) {
        ui->state = US_FAIL;
        return;
    }
    int cfg_val = d.buf[5];
    IfDesc ifs[MAX_IF];
    int n_if = 0;
    parse_config(d.buf, total, ifs, &n_if);
    IfDesc *hub_if = 0;
    for (int i = 0; i < n_if; i++) if (ifs[i].cls == 9) hub_if = &ifs[i];
    if (dev_class == 9 || hub_if) {
        saw_hubs++;
        ui->state = US_HUB;
        ui->cls = 9;
        enumerate_hub(hc, &d, pos, ui, hub_if, cfg_val);
        return;
    }
    int before = ep_count;
    uint32_t add = 0;
    int max_dci = 1;
    for (int i = 0; i < n_if && ep_count < MAX_HID; i++) {
        IfDesc *f = &ifs[i];
        if (f->cls != 3 || !f->ep) continue;
        int kind = HID_TABLET;
        if (f->sub == 1 && f->proto == 1) kind = HID_KBD;
        else if (f->sub == 1 && f->proto == 2) kind = HID_MOUSE;
        HidEp *ep = &eps[ep_count];
        memset(ep, 0, sizeof(*ep));
        if (ring_init(&ep->ring, 16, 1)) break;
        ep->ctrl = ctrl_i;
        ep->slot = d.slot;
        ep->kind = kind;
        ep->iface = f->num;
        ep->dci = (f->ep & 0x0F) * 2 + 1;
        ep->mps = f->mps & 0x7FFu;
        if (ep->mps > sizeof(ep->report)) ep->mps = sizeof(ep->report);
        ep_ctx(hc, &d, pos->speed, f, &ep->ring);
        add |= 1u << ep->dci;
        if (ep->dci > max_dci) max_dci = ep->dci;
        ep_count++;
    }
    if (ep_count == before) {
        ui->state = dev_class == 3 ? US_NOHID : US_SKIP;
        disable_slot(hc, (uint32_t)d.slot);
        return;
    }
    cc = configure(hc, &d, add, max_dci, 0, 0, 0);
    ui->cfg = (uint8_t)cc;
    if (cc != 1) {
        ep_count = before;
        ui->state = US_FAIL;
        disable_slot(hc, (uint32_t)d.slot);
        return;
    }
    cc = control(hc, &d.ep0, d.slot, 0x00, 9, (uint16_t)cfg_val, 0, 0, 0);
    if (cc != 1) ui->cfg = (uint8_t)(100 + cc);
    for (int i = before; i < ep_count; i++) {
        HidEp *ep = &eps[i];
        if (ep->kind == HID_TABLET) {
            int rlen = 0;
            for (int k = 0; k < n_if; k++) if (ifs[k].num == ep->iface) rlen = ifs[k].rlen;
            if (rlen > 256) rlen = 256;
            ep->kind = HID_NONE;
            if (rlen >= 4 && control(hc, &d.ep0, d.slot, 0x81, 6, 0x2200, (uint16_t)ep->iface,
                                     d.buf, (uint16_t)rlen) == 1 &&
                looks_tablet(d.buf, rlen))
                ep->kind = HID_TABLET;
            if (ep->kind == HID_NONE) continue;
        } else {
            control(hc, &d.ep0, d.slot, 0x21, 0x0B, 0, (uint16_t)ep->iface, 0, 0);
            control(hc, &d.ep0, d.slot, 0x21, 0x0A, 0, (uint16_t)ep->iface, 0, 0);
        }
        ui->kinds |= (uint8_t)(1u << ep->kind);
        arm_interrupt(hc, ep);
        serial_write(ep->kind == HID_KBD ? "XHCI: keyboard\n" :
                     ep->kind == HID_TABLET ? "XHCI: tablet\n" : "XHCI: mouse\n");
    }
    ui->state = ui->kinds ? US_HID : US_NOHID;
    note_status();
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
    if (!pages_ready() || phys < 0x200000ULL || phys >= 0x100000000ULL) return;
    uint64_t base = phys & ~0x1FFFFFULL;
    uint32_t gi = (uint32_t)((base >> 30) & 3ULL);
    uint32_t idx = (uint32_t)((base >> 21) & 0x1FFULL);
    volatile uint64_t *pd = (volatile uint64_t *)(0x1802000u + gi * 4096u);
    pd[idx] = base | 0x83ULL | 0x18ULL;
    tlb_flush();
}

void xhci_force_uc(uint32_t phys) { force_uc(phys); }

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
    trace_open(bus, dev, fn, hc);
    hc->cap = (volatile uint8_t *)(uintptr_t)mmio;
    uint32_t cap0 = rd((volatile uint32_t *)hc->cap);
    uint8_t caplen = (uint8_t)cap0;
    if (caplen < 0x10 || caplen > 0x40) {
        trace_snap(TR_MAP);
        return -1;
    }
    hc->op = (volatile uint32_t *)(hc->cap + caplen);
    uint32_t hcs1 = rd((volatile uint32_t *)(hc->cap + 4));
    uint32_t hcs2 = rd((volatile uint32_t *)(hc->cap + 8));
    uint32_t hcc = rd((volatile uint32_t *)(hc->cap + 0x10));
    uint32_t dboff = rd((volatile uint32_t *)(hc->cap + 0x14)) & ~3u;
    uint32_t rtsoff = rd((volatile uint32_t *)(hc->cap + 0x18)) & ~0x1Fu;
    if (dboff < 0x100 || rtsoff < 0x100) {
        trace_snap(TR_MAP);
        return -1;
    }
    hc->db = (volatile uint32_t *)(hc->cap + dboff);
    hc->rt = (volatile uint32_t *)(hc->cap + rtsoff + 0x20);
    hc->ctx_size = (hcc & 4u) ? 64u : 32u;
    uint32_t slots = hcs1 & 0xFFu;
    uint32_t ports = (hcs1 >> 24) & 0xFFu;
    if (!slots || !ports) {
        trace_snap(TR_MAP);
        return -1;
    }
    if (slots > 64) slots = 64;
    if (ports > 32) ports = 32;
    hc->slots = slots;
    hc->ports = ports;
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
        trace_snap((rd(hc->op + 1) & 0x800u) ? TR_CNR : TR_HALT);
        return -2;
    }
    if ((rd(hc->op + 2) & 1u) == 0) {
        trace_snap(TR_PAGE);
        return -3;
    }
    uint32_t scratch = xhci_scratchpad_count(hcs2);
    uint32_t psz = rd(hc->op + 2);
    uint32_t page_size = 4096;
    if (psz) {
        uint32_t bit = 0;
        while (bit < 20 && ((psz >> bit) & 1u) == 0) bit++;
        if (bit < 20) page_size = 1u << (bit + 12);
    }
    hc->dcbaa = halloc((slots + 1u) * 8u, 64);
    if (!hc->dcbaa) {
        trace_snap(TR_RING);
        return -4;
    }
    memset(hc->dcbaa, 0, (slots + 1u) * 8u);
    if (scratch) {
        /* One contiguous run. mem_alloc returns the same address every call,
         * so a loop of it makes every scratchpad page alias the array. */
        uint64_t *spa = halloc(scratch * 8u, 64);
        uint8_t *pages = halloc(scratch * page_size, page_size);
        if (!pages) pages = mem_alloc(scratch * page_size, page_size);
        if (!spa || !pages) {
            trace_snap(TR_SCRATCH);
            return -5;
        }
        memset(spa, 0, scratch * 8u);
        memset(pages, 0, scratch * page_size);
        flush_range(pages, scratch * page_size);
        for (uint32_t i = 0; i < scratch; i++)
            spa[i] = (uint64_t)(uintptr_t)(pages + (uint32_t)i * page_size);
        hc->dcbaa[0] = (uint64_t)(uintptr_t)spa;
        flush_range(spa, scratch * 8u);
    }
    flush_range(hc->dcbaa, (slots + 1u) * 8u);
    if (ring_init(&hc->cmd, 32, 1) || ring_init(&hc->event, 128, 0)) {
        trace_snap(TR_RING);
        return -6;
    }
    uint32_t *erst = halloc(16, 64);
    if (!erst) {
        trace_snap(TR_RING);
        return -7;
    }
    erst[0] = (uint32_t)(uintptr_t)hc->event.trb;
    erst[1] = 0;
    erst[2] = hc->event.size;
    erst[3] = 0;
    flush_range(erst, 16);
    wr(hc->op + (0x38 / 4), slots);
    uint32_t cmd_ring = (uint32_t)(uintptr_t)hc->cmd.trb;
    wr64(hc->op + (0x18 / 4), cmd_ring | 1u);
    wr64(hc->op + (0x30 / 4), (uint32_t)(uintptr_t)hc->dcbaa);
    wr(hc->rt + (0x08 / 4), 1);
    wr64(hc->rt + (0x18 / 4), (uint32_t)(uintptr_t)hc->event.trb);
    wr64(hc->rt + (0x10 / 4), (uint32_t)(uintptr_t)erst);
    wr(hc->rt, 0x2);
    uint32_t crcr_now = rd(hc->op + (0x18 / 4));
    if ((crcr_now & ~0x3Fu) != (cmd_ring & ~0x3Fu))
        trace_snap(TR_CRCR);
    wr(hc->op, rd(hc->op) | 1u | 4u);
    for (int i = 0; i < 100; i++) {
        if ((rd(hc->op + 1) & 1u) == 0) break;
        wait_ms(10);
    }
    if (rd(hc->op + 1) & 1u) {
        trace_snap((rd(hc->op + 1) & 0x800u) ? TR_CNR : TR_RUN);
        return -8;
    }
    if (rd(hc->op + 1) & 0x1000u) {
        trace_snap(TR_RUN);
        return -8;
    }
    hc->alive = 1;
    ctrl_count++;
    hc->usb3 = usb3_port_mask((volatile uint8_t *)hc->cap);
    int saw_ccs = 0;
    for (int i = 0; i < 20; i++) {
        saw_ccs = 0;
        for (uint32_t p = 0; p < ports; p++) {
            volatile uint32_t *psc = hc->op + (0x400 / 4) + p * 4;
            uint32_t v = rd(psc);
            if ((v & (1u << 9)) == 0) wr(psc, 1u << 9);
            if (v & 1u) saw_ccs = 1;
        }
        if (!saw_ccs && i >= 10) break;
        if (saw_ccs && i >= 15) break;
        wait_ms(10);
    }
    if (!saw_ccs) {
        trace_snap(TR_NOCCS);
        note_status();
        return 0;
    }
    for (uint32_t p = 0; p < ports; p++) {
        if (cur_trace && cur_trace->timeout) break;
        volatile uint32_t *psc = hc->op + (0x400 / 4) + p * 4;
        if ((rd(psc) & 1u) == 0) continue;
        int speed = port_reset(hc, (int)p, (hc->usb3 & (1u << p)) != 0);
        DevPos pos;
        memset(&pos, 0, sizeof(pos));
        pos.root = (int)p;
        pos.speed = speed;
        if (speed < 1) {
            if (cur_trace && !cur_trace->have_code && !cur_trace->timeout && (last_portsc & 1u)) {
                cur_trace->portsc = last_portsc;
                trace_snap(TR_PORT);
            }
            if (last_portsc & 1u) {
                UsbInfo *bad = info_new(hc, &pos);
                bad->state = US_RESET;
            }
            continue;
        }
        saw_devs++;
        serial_write("XHCI: port ");
        serial_dec(p + 1);
        serial_write(" speed ");
        serial_dec((uint32_t)speed);
        serial_write("\n");
        enumerate_device(hc, &pos);
    }
    if (cur_trace && !cur_trace->have_code && !cur_trace->timeout &&
        cur_trace->stage != TR_PORT && cur_trace->stage != TR_CRCR)
        trace_snap(saw_ccs ? TR_PORT : TR_NOCCS);
    note_status();
    return 0;
}

static XhciHandoff handoffs[4];
static int handoff_count;

int xhci_handoff_count(void) { return handoff_count; }
const XhciHandoff *xhci_handoff_at(int index) {
    if (index < 0 || index >= handoff_count) return 0;
    return &handoffs[index];
}

static uint32_t page_size_of(uint32_t psz) {
    if (!psz) return 4096;
    uint32_t bit = 0;
    while (bit < 20 && ((psz >> bit) & 1u) == 0) bit++;
    if (bit >= 20) return 4096;
    return 1u << (bit + 12);
}

static void note_scratch(volatile uint8_t *cap, XhciHandoff *rec) {
    uint32_t cap0 = *(volatile uint32_t *)cap;
    uint8_t caplen = (uint8_t)cap0;
    if (caplen < 0x10 || caplen > 0x40) return;
    uint32_t hcs2 = *(volatile uint32_t *)(cap + 8);
    volatile uint32_t *op = (volatile uint32_t *)(cap + caplen);
    rec->scratch = xhci_scratchpad_count(hcs2);
    rec->page_size = page_size_of(op[2]);
}

void xhci_handoff_all(void) {
    handoff_count = 0;
    for (int bus = 0; bus < 256 && handoff_count < 4; bus++) {
        for (int dev = 0; dev < 32 && handoff_count < 4; dev++) {
            for (int fn = 0; fn < 8 && handoff_count < 4; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if (id == 0xFFFFFFFFu || id == 0) {
                    if (fn == 0) break;
                    continue;
                }
                uint32_t cls = pci_read(bus, dev, fn, 8);
                int xhci = (uint8_t)(cls >> 24) == 0x0C && (uint8_t)(cls >> 16) == 0x03 &&
                           (uint8_t)(cls >> 8) == 0x30;
                if (xhci) {
                    XhciHandoff *rec = &handoffs[handoff_count];
                    memset(rec, 0, sizeof(*rec));
                    rec->bus = (uint8_t)bus;
                    rec->dev = (uint8_t)dev;
                    rec->fn = (uint8_t)fn;
                    rec->vendor = (uint16_t)id;
                    rec->device = (uint16_t)(id >> 16);
                    uint32_t lo = pci_read(bus, dev, fn, 0x10);
                    uint32_t hi = ((lo & 7u) == 4u) ? pci_read(bus, dev, fn, 0x14) : 0;
                    rec->bar_lo = lo & ~0xFu;
                    rec->bar_hi = hi;
                    uint64_t phys = ((uint64_t)hi << 32) | (lo & ~0xFu);
                    uint16_t cmd = (uint16_t)pci_read(bus, dev, fn, 4);
                    pci_write16(bus, dev, fn, 4, (uint16_t)(cmd | 0x02u));
                    uint32_t mmio = 0;
                    if (phys && phys < 0x100000000ull) {
                        force_uc(phys);
                        mmio = (uint32_t)phys;
                    } else if (phys) {
                        mmio = map_high_bar(phys);
                    }
                    serial_write("XHCI: handoff ");
                    serial_hex32((uint32_t)((bus << 16) | (dev << 8) | fn));
                    serial_write(" mmio ");
                    serial_hex32(mmio);
                    serial_write("\n");
                    if (mmio) {
                        volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)mmio;
                        legacy_handoff(cap, rec);
                        note_scratch(cap, rec);
                    }
                    handoff_count++;
                }
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    if (!handoff_count) serial_write("XHCI: handoff none\n");
}

void xhci_init(void) {
    uint32_t started = timer_ticks();
    ctrl_count = 0;
    ep_count = 0;
    usb_info_n = 0;
    trace_n = 0;
    cur_trace = 0;
    saw_ctl = saw_ports = saw_devs = saw_hubs = 0;
    next_window = 0xF2000000u;
    heap = XHCI_HEAP;
    if (bootinfo()->magic == BOOTINFO_MAGIC && bootinfo()->xhci_heap)
        heap = (uint8_t *)(uintptr_t)bootinfo()->xhci_heap;
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
                    trace_open(bus, dev, fn, 0);
                    if (cur_trace) cur_trace->stage = TR_MAP;
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
    else if (!saw_ctl) set_status("USB BAR high");
    else note_status();
    serial_write("XHCI: ");
    serial_write(status_line);
    serial_write(" init ms ");
    serial_dec((timer_ticks() - started) * 10u);
    serial_write("\n");
    for (int i = 0; i < usb_info_n; i++) {
        serial_write("USB ");
        serial_write(xhci_usb_line(i));
        serial_write("\n");
    }
}

const char *xhci_cmd_line(int bus, int dev, int fn) {
    static char line[96];
    const CmdTrace *t = 0;
    for (int i = 0; i < trace_n; i++) {
        if (traces[i].bus == (uint8_t)bus && traces[i].dev == (uint8_t)dev &&
            traces[i].fn == (uint8_t)fn) {
            t = &traces[i];
            break;
        }
    }
    char *p = line;
    char *end = line + sizeof(line) - 1;
    if (!t) {
        p = put_s(p, end, "STS - CRCR - ES - none");
        *p = 0;
        return line;
    }
    p = put_s(p, end, "STS ");
    p = put_x(p, end, t->usbsts, 8);
    p = put_s(p, end, " CRCR ");
    p = put_x(p, end, t->crcr, 8);
    p = put_s(p, end, " ES ");
    if (t->timeout) p = put_s(p, end, "t");
    else if (!t->have_code) p = put_s(p, end, "-");
    else p = put_u(p, end, t->code);
    p = put_s(p, end, " ");
    p = put_s(p, end, tr_name[t->stage < 13 ? t->stage : 0]);
    if (t->stage == TR_PORT && t->portsc) {
        p = put_s(p, end, " PSC ");
        p = put_x(p, end, t->portsc, 8);
    }
    *p = 0;
    return line;
}

int xhci_usb_count(void) { return usb_info_n; }
uint32_t xhci_kbd_reports(void) { return kbd_reports; }
uint32_t xhci_mouse_reports(void) { return mouse_reports; }
uint32_t xhci_hid_errors(void) { return hid_errors; }
int xhci_hid_last_error(void) { return hid_last_err; }

/* x0 p2.3 s3 sl4 m1 B1 G8:1 AD1 GD1 0C45:7D19 cls00 kbd */
const char *xhci_usb_line(int index) {
    static char line[128];
    if (index < 0 || index >= usb_info_n) return "";
    const UsbInfo *u = &usb_info[index];
    char *p = line;
    char *end = line + sizeof(line) - 1;
    p = put_s(p, end, "x");
    p = put_u(p, end, u->ctrl);
    p = put_s(p, end, " p");
    p = put_u(p, end, u->root);
    if (u->hport) {
        p = put_s(p, end, ".");
        p = put_u(p, end, u->hport);
    }
    p = put_s(p, end, " s");
    p = put_u(p, end, u->speed);
    if (u->state == US_RESET) {
        p = put_s(p, end, " RESET-FAIL ps");
        p = put_x(p, end, u->psc, 8);
        *p = 0;
        return line;
    }
    p = put_s(p, end, " slot");
    p = put_u(p, end, u->slot);
    if (u->tries > 1) {
        p = put_s(p, end, " [m");
        p = put_u(p, end, u->prev_scheme);
        p = put_s(p, end, " B");
        p = put_cc(p, end, u->prev_ad1);
        p = put_s(p, end, " G8:");
        p = put_cc(p, end, u->prev_gd8);
        p = put_s(p, end, " AD");
        p = put_cc(p, end, u->prev_ad2);
        p = put_s(p, end, "]");
    }
    p = put_s(p, end, " m");
    p = put_u(p, end, u->scheme);
    p = put_s(p, end, " ES");
    p = put_cc(p, end, u->es);
    if (u->scheme == 1) {
        p = put_s(p, end, " B");
        p = put_cc(p, end, u->ad1);
    }
    p = put_s(p, end, " G8:");
    p = put_cc(p, end, u->gd8);
    p = put_s(p, end, " AD");
    p = put_cc(p, end, u->ad2);
    p = put_s(p, end, " GD");
    p = put_cc(p, end, u->gd18);
    if (u->gd18 == 1) {
        p = put_s(p, end, " ");
        p = put_x(p, end, u->vid, 4);
        p = put_s(p, end, ":");
        p = put_x(p, end, u->pid, 4);
        p = put_s(p, end, " cls");
        p = put_x(p, end, u->cls, 2);
        p = put_s(p, end, " CF");
        p = put_cc(p, end, u->cfg);
    }
    switch (u->state) {
    case US_HID:
        if (u->kinds & (1u << HID_KBD)) p = put_s(p, end, " KBD");
        if (u->kinds & (1u << HID_MOUSE)) p = put_s(p, end, " MOUSE");
        if (u->kinds & (1u << HID_TABLET)) p = put_s(p, end, " TABLET");
        break;
    case US_HUB:
        p = put_s(p, end, " HUB n");
        p = put_u(p, end, u->nports);
        break;
    case US_SKIP:
        p = put_s(p, end, " skip");
        break;
    case US_NOHID:
        p = put_s(p, end, " hid-noboot");
        break;
    default:
        p = put_s(p, end, " FAIL st");
        p = put_u(p, end, u->sst);
        p = put_s(p, end, "/");
        p = put_u(p, end, u->est);
        p = put_s(p, end, " ps");
        p = put_x(p, end, u->psc, 8);
        break;
    }
    *p = 0;
    return line;
}

void xhci_poll(void) {
    if (!ctrls || !eps) return;
    for (int c = 0; c < ctrl_count; c++) {
        Ctrl *hc = &ctrls[c];
        if (!hc->alive) continue;
        Trb ev;
        int guard = 0;
        while (guard++ < 64 && event_poll(hc, &ev)) dispatch(hc, &ev);
        for (int i = 0; i < ep_count; i++) {
            HidEp *ep = &eps[i];
            if (ep->ctrl != c || !ep->need_reset) continue;
            ep->need_reset = 0;
            if (ep->resets++ > 50) continue;
            recover_ep(hc, &ep->ring, ep->slot, ep->dci);
            arm_interrupt(hc, ep);
        }
        for (int i = 0; i < ep_count; i++) {
            HidEp *ep = &eps[i];
            if (ep->ctrl != c || ep->kind != HID_KBD) continue;
            uint8_t held = 0;
            for (int k = 0; k < 6; k++) {
                uint8_t key = ep->prev[2 + k];
                if (key >= 4 && key < 0xE0) { held = key; break; }
            }
            if (!held) { ep->repeat_at = 0; continue; }
            uint32_t now = timer_ticks();
            if (!ep->repeat_at) { ep->repeat_at = now + 40; continue; }
            if ((int32_t)(now - ep->repeat_at) < 0) continue;
            ep->repeat_at = now + 8;
            post_set1(hid_set1[held], 0);
        }
    }
}
