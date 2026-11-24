#include "i2chid.h"
#include "i2cscan.h"
#include "io.h"
#include "gfx.h"
#include "input.h"
#include "serial.h"
#include "xhci.h"

/* ---- ACPI: the touchpad and its I2C controller, found by i2cscan ---- */

static I2cCand *cands;
static int n_cand;
static int n_tables;

/* ---- AMD FCH: AOAC power, GPIO level ---- */

#define ACPI_MMIO 0xFED80000u
#define AMD_GPIO (ACPI_MMIO + 0x1500u)
#define AMD_AOAC (ACPI_MMIO + 0x1E00u)

static int amd_cpu;

static int cpu_is_amd(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    return b == 0x68747541u && d == 0x69746E65u && c == 0x444D4163u;
}

static int aoac_id(uint32_t base) {
    switch (base) {
    case 0xFEDC2000u: return 5;
    case 0xFEDC3000u: return 6;
    case 0xFEDC4000u: return 7;
    case 0xFEDC5000u: return 8;
    case 0xFEDC6000u: return 9;
    case 0xFEDCB000u: return 10;
    default: return -1;
    }
}

static uint8_t aoac_state(int id) {
    return *(volatile uint8_t *)(uintptr_t)(AMD_AOAC + 0x41u + (uint32_t)id * 2u);
}

/* coreboot power_on_aoac_device: set PwrOnDev, wait for PwrRst and RstClkOk. */
static void aoac_on(int id) {
    volatile uint8_t *ctl = (volatile uint8_t *)(uintptr_t)(AMD_AOAC + 0x40u + (uint32_t)id * 2u);
    *ctl = (uint8_t)(*ctl | 0x08u);
    for (int i = 0; i < 50; i++) {
        if ((aoac_state(id) & 3u) == 3u) break;
        timer_wait_ms(1);
    }
}

static int gpio_level(int pin) {
    if (pin < 0 || pin > 255 || !amd_cpu) return -1;
    uint32_t v = *(volatile uint32_t *)(uintptr_t)(AMD_GPIO + (uint32_t)pin * 4u);
    return (v >> 16) & 1;
}

/* ---- Synopsys DesignWare I2C master, polled (Linux i2c-designware) ---- */

#define IC_CON 0x00
#define IC_TAR 0x04
#define IC_DATA_CMD 0x10
#define IC_SS_HCNT 0x14
#define IC_SS_LCNT 0x18
#define IC_FS_HCNT 0x1C
#define IC_FS_LCNT 0x20
#define IC_INTR_MASK 0x30
#define IC_RAW_INTR 0x34
#define IC_RX_TL 0x38
#define IC_TX_TL 0x3C
#define IC_CLR_INTR 0x40
#define IC_CLR_TX_ABRT 0x54
#define IC_CLR_STOP 0x60
#define IC_ENABLE 0x6C
#define IC_STATUS 0x70
#define IC_RXFLR 0x78
#define IC_SDA_HOLD 0x7C
#define IC_ABRT_SRC 0x80
#define IC_EN_STATUS 0x9C
#define IC_PARAM1 0xF4
#define IC_VERSION 0xF8
#define IC_TYPE 0xFC
#define DW_TYPE 0x44570140u

typedef struct {
    uint32_t base;
    uint32_t rx_depth, tx_depth;
    uint32_t abrt;
    int fail;
} Dw;

static uint32_t dr(Dw *d, uint32_t off) { return *(volatile uint32_t *)(uintptr_t)(d->base + off); }
static void dwr(Dw *d, uint32_t off, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(d->base + off) = v; }

static int dw_enable(Dw *d, int on) {
    for (int i = 0; i < 200; i++) {
        dwr(d, IC_ENABLE, on ? 1u : 0u);
        if ((dr(d, IC_EN_STATUS) & 1u) == (uint32_t)(on ? 1 : 0)) return 0;
        uint64_t t = timer_us() + 25;
        while (timer_us() < t) {}
    }
    return -1;
}

/* 150 MHz input clock (Linux acpi_apd wt_i2c_desc for AMDI0010), 100 kHz. */
static int dw_init(Dw *d) {
    if (dr(d, IC_TYPE) != DW_TYPE) return -1;
    if (dw_enable(d, 0)) return -2;
    uint32_t p1 = dr(d, IC_PARAM1);
    d->tx_depth = p1 ? ((p1 >> 16) & 0xFFu) + 1u : 16u;
    d->rx_depth = p1 ? ((p1 >> 8) & 0xFFu) + 1u : 16u;
    if (d->rx_depth < 2) d->rx_depth = 2;
    dwr(d, IC_SS_HCNT, 642);
    dwr(d, IC_SS_LCNT, 749);
    dwr(d, IC_FS_HCNT, 132);
    dwr(d, IC_FS_LCNT, 239);
    if (dr(d, IC_VERSION) >= 0x3131312Au && (dr(d, IC_SDA_HOLD) & 0xFFFFu) == 0)
        dwr(d, IC_SDA_HOLD, 45);
    dwr(d, IC_TX_TL, 0);
    dwr(d, IC_RX_TL, 0);
    /* Master, standard speed, restart enable, slave disable. */
    dwr(d, IC_CON, 0x63);
    dwr(d, IC_INTR_MASK, 0);
    return 0;
}

/* Write wn bytes, then read rn with a repeated start. 0 on success. */
static int dw_xfer(Dw *d, uint16_t addr, const uint8_t *w, int wn, uint8_t *r, int rn) {
    if (dw_enable(d, 0)) return -5;
    dwr(d, IC_TAR, addr & 0x3FFu);
    dwr(d, IC_INTR_MASK, 0);
    (void)dr(d, IC_CLR_INTR);
    if (dw_enable(d, 1)) return -5;
    int total = wn + rn;
    int sent = 0, got = 0;
    uint64_t deadline = timer_us() + 20000u + (uint64_t)total * 300u;
    d->abrt = 0;
    while (got < rn || sent < total) {
        uint32_t raw = dr(d, IC_RAW_INTR);
        if (raw & (1u << 6)) {
            d->abrt = dr(d, IC_ABRT_SRC);
            (void)dr(d, IC_CLR_TX_ABRT);
            dw_enable(d, 0);
            return -1;
        }
        while (sent < total && (dr(d, IC_STATUS) & 2u)) {
            if (sent >= wn && (uint32_t)(sent - wn - got) >= d->rx_depth - 1u) break;
            uint32_t cmd;
            if (sent < wn) {
                cmd = w[sent];
            } else {
                cmd = 0x100u;
                if (sent == wn && wn > 0) cmd |= 0x400u;
            }
            if (sent == total - 1) cmd |= 0x200u;
            dwr(d, IC_DATA_CMD, cmd);
            sent++;
        }
        while (got < rn && dr(d, IC_RXFLR)) r[got++] = (uint8_t)dr(d, IC_DATA_CMD);
        if (timer_us() > deadline) {
            dw_enable(d, 0);
            return -2;
        }
    }
    for (;;) {
        uint32_t raw = dr(d, IC_RAW_INTR);
        if (raw & (1u << 6)) {
            d->abrt = dr(d, IC_ABRT_SRC);
            (void)dr(d, IC_CLR_TX_ABRT);
            dw_enable(d, 0);
            return -1;
        }
        if (raw & (1u << 9)) break;
        if (timer_us() > deadline) {
            dw_enable(d, 0);
            return -3;
        }
    }
    (void)dr(d, IC_CLR_STOP);
    dw_enable(d, 0);
    return 0;
}

/* ---- I2C-HID ---- */

static Dw bus;
static int ready;
static uint16_t slave;
static uint16_t hidreg;
static uint8_t hdesc[30];
static int desc_ok;
static uint16_t rlen_hdr, rreg, inreg, maxin, cmdreg, vid, pid;
static int rlen_got;
#define RDESC_MAX 2048
#define INBUF_MAX 256
static uint8_t *rdesc;
static uint8_t *inbuf;
static uint16_t *bitpos;
static uint32_t packets, empty_reads, read_errs;
static uint8_t last_pkt[12];
static int last_pkt_n;
static int gpio_pin = -1;
static uint8_t gpio_flags;
static int dsm_val = -1;
static const I2cCand *chosen;
static char ctl_name[5];
static uint32_t ctl_base;
static uint8_t aoac_before = 0xFF, aoac_after = 0xFF;
static int init_stage;
static int reset_ok, power_ok;
static uint32_t ctl_type, ctl_ver;
static uint64_t next_poll;

static int hid_read(uint16_t reg, uint8_t *buf, int n) {
    uint8_t w[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
    return dw_xfer(&bus, slave, w, 2, buf, n);
}

static int hid_cmd(uint8_t opcode, uint8_t arg) {
    uint8_t w[4] = {(uint8_t)cmdreg, (uint8_t)(cmdreg >> 8), arg, opcode};
    return dw_xfer(&bus, slave, w, 4, 0, 0);
}

static int desc_valid(const uint8_t *h) {
    return h[0] == 30 && h[1] == 0 && h[2] == 0x00 && h[3] == 0x01;
}

/* ---- HID report descriptor: the mouse collection, or the first finger ---- */

typedef struct {
    int have;
    uint8_t id;
    uint16_t x_off, y_off, b_off, tip_off;
    uint8_t x_size, y_size, b_count, has_tip, rel;
    int32_t x_max, y_max;
    uint8_t x_signed, y_signed;
} Layout;

static Layout mouse_l, tp_l;
static int uses_ids;

static void field(uint32_t app, int finger, uint32_t u, uint32_t rid, uint16_t off, uint32_t size,
                  int rel, int32_t lmin, int32_t lmax) {
    int mouse = app == 0x00010002u || app == 0x00010001u;
    int tp = app == 0x000D0005u;
    Layout *l = mouse ? &mouse_l : tp ? &tp_l : 0;
    if (!l) return;
    if (tp && u >> 16 != 9 && finger != 1) return;
    if (l->have && l->id != (uint8_t)rid) return;
    if (size > 32) return;
    if (u == 0x00010030u && !l->x_size) {
        l->x_off = off; l->x_size = (uint8_t)size; l->x_max = lmax; l->x_signed = lmin < 0; l->rel = (uint8_t)rel;
    } else if (u == 0x00010031u && !l->y_size) {
        l->y_off = off; l->y_size = (uint8_t)size; l->y_max = lmax; l->y_signed = lmin < 0;
    } else if (u >> 16 == 9 && size == 1) {
        if (!l->b_count) l->b_off = off;
        if (l->b_count < 3) l->b_count++;
    } else if (u == 0x000D0042u && !l->has_tip) {
        l->tip_off = off; l->has_tip = 1;
    } else {
        return;
    }
    l->have = 1;
    l->id = (uint8_t)rid;
}

static void parse_rdesc(const uint8_t *d, int n) __attribute__((noinline));
static void parse_rdesc(const uint8_t *d, int n) {
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16];
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    int range = 0;
    uint32_t app = 0;
    int depth = 0, finger = 0;
    memset(bitpos, 0, 256 * sizeof(uint16_t));
    memset(&mouse_l, 0, sizeof(mouse_l));
    memset(&tp_l, 0, sizeof(tp_l));
    uses_ids = 0;
    for (int i = 0; i < n; ) {
        uint8_t b = d[i];
        if (b == 0xFE) {
            if (i + 1 >= n) break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = b & 3;
        if (size == 3) size = 4;
        int type = (b >> 2) & 3;
        int tag = b >> 4;
        if (i + 1 + size > n) break;
        uint32_t v = 0;
        for (int k = 0; k < size; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sv = (int32_t)v;
        if (size == 1) sv = (int8_t)v;
        else if (size == 2) sv = (int16_t)v;
        i += 1 + size;
        if (type == 1) {
            switch (tag) {
            case 0: page = v; break;
            case 1: lmin = sv; break;
            case 2: lmax = (lmin >= 0 && size < 4) ? (int32_t)v : sv; break;
            case 7: rsize = v; break;
            case 8: rid = v & 0xFFu; uses_ids = 1; break;
            case 9: rcount = v; break;
            default: break;
            }
        } else if (type == 2) {
            uint32_t full = size == 4 ? v : ((page << 16) | v);
            if (tag == 0 && nu < 16) usages[nu++] = full;
            else if (tag == 1) { umin = full; range = 1; }
            else if (tag == 2) { umax = full; range = 1; }
        } else if (type == 0) {
            if (tag == 0xA) {
                uint32_t u = nu ? usages[0] : (range ? umin : 0);
                if (depth == 0) {
                    app = u;
                    finger = 0;
                } else if (app == 0x000D0005u && u == 0x000D0022u) {
                    finger++;
                }
                depth++;
            } else if (tag == 0xC) {
                if (depth > 0) depth--;
            } else if (tag == 8) {
                for (uint32_t k = 0; k < rcount && k < 64; k++) {
                    uint32_t u = 0;
                    if (nu) u = usages[k < (uint32_t)nu ? k : (uint32_t)nu - 1];
                    else if (range) u = umin + k <= umax ? umin + k : umax;
                    if ((v & 1u) == 0 && (v & 2u))
                        field(app, finger, u, rid, bitpos[rid], rsize, (v & 4u) != 0, lmin, lmax);
                    bitpos[rid] = (uint16_t)(bitpos[rid] + rsize);
                }
                if (rcount > 64) bitpos[rid] = (uint16_t)(bitpos[rid] + (rcount - 64) * rsize);
            }
            nu = 0;
            range = 0;
        }
    }
}

static int32_t bits(const uint8_t *p, int len, uint32_t off, uint32_t size, int sign) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < size; k++) {
        uint32_t bit = off + k;
        if ((int)(bit >> 3) >= len) break;
        if (p[bit >> 3] & (1u << (bit & 7))) v |= 1u << k;
    }
    if (sign && size < 32 && (v & (1u << (size - 1)))) v |= ~((1u << size) - 1u);
    return (int32_t)v;
}

static int tp_prev_tip, tp_px, tp_py;

static void handle_report(const uint8_t *r, int n) {
    if (n < 1) return;
    uint8_t id = uses_ids ? r[0] : 0;
    const uint8_t *p = uses_ids ? r + 1 : r;
    int len = uses_ids ? n - 1 : n;
    const Layout *l = 0;
    if (mouse_l.have && mouse_l.id == id) l = &mouse_l;
    else if (tp_l.have && tp_l.id == id) l = &tp_l;
    if (!l || !l->x_size || !l->y_size) return;
    int btn = 0;
    for (int k = 0; k < l->b_count; k++) btn |= bits(p, len, l->b_off + k, 1, 0) << k;
    int32_t x = bits(p, len, l->x_off, l->x_size, l->x_signed);
    int32_t y = bits(p, len, l->y_off, l->y_size, l->y_signed);
    if (l == &mouse_l && l->rel) {
        input_post_mouse(x, y, btn);
        return;
    }
    int tip = l->has_tip ? bits(p, len, l->tip_off, 1, 0) : 1;
    int dx = 0, dy = 0;
    if (tip && tp_prev_tip) {
        int xm = l->x_max > 0 ? l->x_max : 4096;
        int ym = l->y_max > 0 ? l->y_max : 4096;
        dx = (x - tp_px) * gfx_width() / xm;
        dy = (y - tp_py) * gfx_height() / ym;
    }
    tp_prev_tip = tip;
    tp_px = x;
    tp_py = y;
    input_post_mouse(dx, dy, btn);
}

/* ---- bring-up ---- */

static int try_controller(uint32_t base) {
    if (!base) return 0;
    xhci_force_uc(base);
    ctl_base = base;
    bus.base = base;
    ctl_type = dr(&bus, IC_TYPE);
    if (ctl_type != DW_TYPE && amd_cpu) {
        int id = aoac_id(base);
        if (id >= 0) {
            aoac_before = aoac_state(id);
            aoac_on(id);
            aoac_after = aoac_state(id);
            timer_wait_ms(5);
            ctl_type = dr(&bus, IC_TYPE);
        }
    } else if (amd_cpu && aoac_id(base) >= 0) {
        aoac_before = aoac_after = aoac_state(aoac_id(base));
    }
    ctl_ver = dr(&bus, IC_VERSION);
    if (ctl_type != DW_TYPE) return 0;
    return dw_init(&bus) == 0;
}

static int probe_slave(uint16_t addr, int dsm) {
    slave = addr;
    int regs[7] = {dsm, 0x20, 0x01, 0x30, 0x10, 0x00, 0x0F};
    for (int k = 0; k < 7; k++) {
        if (regs[k] < 0) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) if (regs[j] == regs[k]) dup = 1;
        if (dup) continue;
        memset(hdesc, 0, sizeof(hdesc));
        int rc = hid_read((uint16_t)regs[k], hdesc, 30);
        if (rc == -1 && bus.abrt & 1u) return 0;
        if (rc == 0 && desc_valid(hdesc)) {
            hidreg = (uint16_t)regs[k];
            return 1;
        }
    }
    return 0;
}

/* 1 when the pad has data, 0 when idle, -1 with no readable pin.
 * Polarity comes from the GpioInt flags: 0 is active high, anything
 * else is treated as active low, which is what I2C-HID devices use. */
static int gpio_asserted(void) {
    int lv = gpio_level(gpio_pin);
    if (gpio_pin < 0 || lv < 0) return -1;
    return ((gpio_flags >> 1) & 3) == 0 ? lv : lv == 0;
}

static void wait_int(int ms) {
    for (int i = 0; i < ms; i++) {
        if (gpio_asserted() == 1) return;
        timer_wait_ms(1);
    }
}

static void log_s(const char *s) { serial_write(s); }

void i2chid_init(void) __attribute__((noinline));
void i2chid_init(void) {
    amd_cpu = cpu_is_amd();
    n_cand = 0;
    n_tables = 0;
    init_stage = 0;
    cands = xhci_heap_alloc(sizeof(I2cCand) * I2C_MAX_CAND);
    rdesc = xhci_heap_alloc(RDESC_MAX);
    inbuf = xhci_heap_alloc(INBUF_MAX);
    bitpos = xhci_heap_alloc(256 * sizeof(uint16_t));
    if (!cands || !rdesc || !inbuf || !bitpos) {
        log_s("I2CHID: no memory\n");
        return;
    }
    n_cand = i2cscan_find(cands, I2C_MAX_CAND, &n_tables);
    init_stage = 1;
    if (!n_cand) {
        log_s("I2CHID: no PNP0C50 device in ACPI\n");
        return;
    }
    xhci_force_uc(0xFEC00000u);
    static const uint32_t known[6] = {
        0xFEDC2000u, 0xFEDC3000u, 0xFEDC4000u, 0xFEDC5000u, 0xFEDC6000u, 0xFEDCB000u
    };
    for (int ci = 0; ci < n_cand && !desc_ok; ci++) {
        const I2cCand *c = &cands[ci];
        for (int r = 0; r < c->n_res && !desc_ok; r++) {
            int amd = 0;
            uint32_t base = i2cscan_mmio(c->ctl[r], &amd);
            init_stage = 2;
            for (int k = -1; k < 6 && !desc_ok; k++) {
                uint32_t b = k < 0 ? base : known[k];
                if (k >= 0 && (!amd_cpu || b == base)) continue;
                if (!try_controller(b)) continue;
                init_stage = 3;
                if (probe_slave(c->addr[r], c->dsm)) {
                    desc_ok = 1;
                    chosen = c;
                    for (int j = 0; j < 5; j++) ctl_name[j] = c->ctl[r][j];
                    gpio_pin = c->gpio;
                    gpio_flags = c->gpio_flags;
                    dsm_val = c->dsm;
                }
            }
        }
    }
    if (!chosen) {
        chosen = &cands[0];
        for (int j = 0; j < 5; j++) ctl_name[j] = cands[0].ctl[0][j];
        slave = cands[0].addr[0];
        gpio_pin = cands[0].gpio;
        dsm_val = cands[0].dsm;
        log_s("I2CHID: no HID descriptor\n");
        return;
    }
    init_stage = 4;
    rlen_hdr = (uint16_t)(hdesc[4] | (hdesc[5] << 8));
    rreg = (uint16_t)(hdesc[6] | (hdesc[7] << 8));
    inreg = (uint16_t)(hdesc[8] | (hdesc[9] << 8));
    maxin = (uint16_t)(hdesc[10] | (hdesc[11] << 8));
    cmdreg = (uint16_t)(hdesc[16] | (hdesc[17] << 8));
    vid = (uint16_t)(hdesc[20] | (hdesc[21] << 8));
    pid = (uint16_t)(hdesc[22] | (hdesc[23] << 8));
    if (maxin > INBUF_MAX) maxin = INBUF_MAX;
    power_ok = hid_cmd(0x08, 0x00) == 0;
    timer_wait_ms(30);
    reset_ok = hid_cmd(0x01, 0x00) == 0;
    wait_int(gpio_pin >= 0 ? 1000 : 150);
    if (maxin >= 2) dw_xfer(&bus, slave, 0, 0, inbuf, maxin);
    int want = rlen_hdr;
    if (want > RDESC_MAX) want = RDESC_MAX;
    if (want > 0 && hid_read(rreg, rdesc, want) == 0) {
        rlen_got = want;
        parse_rdesc(rdesc, want);
    }
    init_stage = 5;
    ready = maxin >= 2;
    log_s("I2CHID: ready\n");
}

void i2chid_poll(void) {
    if (!ready) return;
    int asserted = gpio_asserted();
    if (asserted == 0) return;
    if (asserted < 0) {
        uint64_t now = timer_us();
        if (now < next_poll) return;
        next_poll = now + 8000u;
    }
    if (dw_xfer(&bus, slave, 0, 0, inbuf, maxin)) {
        read_errs++;
        return;
    }
    int len = inbuf[0] | (inbuf[1] << 8);
    if (len <= 2 || len > maxin) {
        empty_reads++;
        return;
    }
    packets++;
    last_pkt_n = len - 2 < (int)sizeof(last_pkt) ? len - 2 : (int)sizeof(last_pkt);
    memcpy(last_pkt, inbuf + 2, (size_t)last_pkt_n);
    handle_report(inbuf + 2, len - 2);
}

uint32_t i2chid_packets(void) { return packets; }

/* ---- DIAG ---- */

int i2chid_line_count(void) { return 4; }

const char *i2chid_line(int index) {
    static char line[192];
    line[0] = 0;
    if (index == 0) {
        kfmt(line, sizeof(line),
             "i2c base %8x slv %2x hidreg %4x desc %s rpt %u/%u pkts %u empty %u err %u",
             ctl_base, slave, hidreg, desc_ok ? "ok" : "err", (uint32_t)rlen_got,
             (uint32_t)rlen_hdr, packets, empty_reads, read_errs);
    } else if (index == 1) {
        int n = kfmt(line, sizeof(line), "TP acpi tbl %u cand %u", (uint32_t)n_tables, (uint32_t)n_cand);
        if (chosen) {
            n += kfmt(line + n, (int)sizeof(line) - n, " dev %s sc%u", chosen->name, (uint32_t)chosen->score);
            for (int r = 0; r < chosen->n_res; r++)
                n += kfmt(line + n, (int)sizeof(line) - n, "%s%2x@%s/%uk", r ? "," : " i2c ",
                          chosen->addr[r], chosen->ctl[r], chosen->speed[r] / 1000u);
            if (gpio_pin < 0) n += kfmt(line + n, (int)sizeof(line) - n, " gpio -");
            else {
                int lv = gpio_level(gpio_pin);
                n += kfmt(line + n, (int)sizeof(line) - n, " gpio %u%c%c=%c", (uint32_t)gpio_pin,
                          (gpio_flags & 1) ? 'e' : 'l', ((gpio_flags >> 1) & 3) == 1 ? 'L' : 'H',
                          lv < 0 ? '?' : lv ? '1' : '0');
            }
            if (dsm_val < 0) n += kfmt(line + n, (int)sizeof(line) - n, " dsm ?");
            else n += kfmt(line + n, (int)sizeof(line) - n, " dsm %2x", (uint32_t)dsm_val);
        }
        kfmt(line + n, (int)sizeof(line) - n, " stg %u", (uint32_t)init_stage);
    } else if (index == 2) {
        int n = kfmt(line, sizeof(line), "  dw type %8x ver %8x aoac %2x>%2x fifo %u/%u abrt %5x",
                     ctl_type, ctl_ver, aoac_before, aoac_after, bus.tx_depth, bus.rx_depth, bus.abrt);
        if (desc_ok)
            kfmt(line + n, (int)sizeof(line) - n, " %4x:%4x in %2x/%u cmd %2x%s%s", vid, pid, inreg,
                 (uint32_t)maxin, cmdreg, power_ok ? " pwr" : " PWR!", reset_ok ? " rst" : " RST!");
    } else if (index == 3) {
        int n = kfmt(line, sizeof(line), "  ms ");
        if (mouse_l.have)
            n += kfmt(line + n, (int)sizeof(line) - n, "id%2x x%u/%u%c b%u", mouse_l.id,
                      (uint32_t)mouse_l.x_off, (uint32_t)mouse_l.x_size, mouse_l.rel ? 'r' : 'a',
                      (uint32_t)mouse_l.b_count);
        else n += kfmt(line + n, (int)sizeof(line) - n, "-");
        n += kfmt(line + n, (int)sizeof(line) - n, " tp ");
        if (tp_l.have)
            n += kfmt(line + n, (int)sizeof(line) - n, "id%2x x%u/%u max%u", tp_l.id,
                      (uint32_t)tp_l.x_off, (uint32_t)tp_l.x_size,
                      (uint32_t)(tp_l.x_max > 0 ? tp_l.x_max : 0));
        else n += kfmt(line + n, (int)sizeof(line) - n, "-");
        n += kfmt(line + n, (int)sizeof(line) - n, " last");
        for (int k = 0; k < last_pkt_n && k < 10; k++)
            n += kfmt(line + n, (int)sizeof(line) - n, " %2x", last_pkt[k]);
    }
    return line;
}
