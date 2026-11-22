#include "diag.h"
#include "buildid.h"
#include "bootinfo.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "pci.h"
#include "serial.h"
#include "xhci.h"
#include "acpi.h"

/* Fixed addresses the kernel uses. Bit i of overlap_mask means address i
 * was not inside a usable UEFI region. */
static const uint32_t watch_addr[] = {
    0x6000u, 0x7000u, 0x7C00u, 0x10000u, 0x12000u,
    0x80000u, 0x90000u, 0u, 0x1800000u, 0x1100000u
};

static char fixed_line[40][96];
static int fixed_count;
static int probed;

static void add_line(const char *text) {
    serial_write(text);
    serial_write("\n");
    if (fixed_count >= (int)(sizeof(fixed_line) / sizeof(fixed_line[0]))) return;
    int i = 0;
    for (; text[i] && i < 95; i++) fixed_line[fixed_count][i] = text[i];
    fixed_line[fixed_count][i] = 0;
    fixed_count++;
}

static void put_hex8(char *dst, int *n, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) dst[(*n)++] = hex[(v >> s) & 0xF];
}

static void put_hex16(char *dst, int *n, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    dst[(*n)++] = hex[(v >> 4) & 0xF];
    dst[(*n)++] = hex[v & 0xF];
}

static void put_hex4(char *dst, int *n, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    dst[(*n)++] = hex[(v >> 12) & 0xF];
    dst[(*n)++] = hex[(v >> 8) & 0xF];
    dst[(*n)++] = hex[(v >> 4) & 0xF];
    dst[(*n)++] = hex[v & 0xF];
}

static void put_dec(char *dst, int *n, uint32_t v) {
    char rev[12];
    int r = 0;
    if (!v) rev[r++] = '0';
    while (v && r < 10) {
        rev[r++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (r) dst[(*n)++] = rev[--r];
}

static int ps2_wait_in(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & 2) == 0) return 0;
    return -1;
}

static int ps2_read_raw(int spins) {
    for (int i = 0; i < spins; i++) {
        uint8_t st = inb(0x64);
        if (st & 1) {
            uint8_t data = inb(0x60);
            input_note_ps2(st, data);
            return data;
        }
    }
    return -1;
}

static void ps2_cmd(uint8_t cmd) {
    if (ps2_wait_in()) return;
    outb(0x64, cmd);
}

static void ps2_data(uint8_t cmd) {
    if (ps2_wait_in()) return;
    outb(0x60, cmd);
}

static void format_bytes(char *dst, int *n, const int *vals, int count) {
    for (int i = 0; i < count; i++) {
        if (*n > 80) return;
        dst[(*n)++] = ' ';
        if (vals[i] < 0) {
            dst[(*n)++] = '-';
            continue;
        }
        put_hex16(dst, n, (uint32_t)vals[i]);
    }
}

static uint16_t pit_now(void) {
    outb(0x43, 0x00);
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}

static uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int pages_ready(void) {
    uint32_t cr0, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (cr0 & 0x80000000u) && ((cr3 & ~0xFFFu) == 0x1800000u);
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

static uint32_t map_high(uint64_t phys, int slot) {
    if (!pages_ready()) return 0;
    uint64_t base = phys & ~0x1FFFFFULL;
    uint32_t off = (uint32_t)(phys - base);
    uint32_t window = 0xF2000000u + (uint32_t)slot * 0x200000u;
    if (window >= 0xF8000000u) return 0;
    volatile uint64_t *pd = (volatile uint64_t *)0x1805000;
    uint32_t idx = (window >> 21) & 0x1FF;
    pd[idx] = base | 0x83ULL | 0x18ULL;
    tlb_flush();
    return window + off;
}

static const char *compl_name(uint32_t code) {
    switch (code) {
    case 1: return "Success";
    case 2: return "Data Buffer Error";
    case 3: return "Babble";
    case 4: return "USB Transaction Error";
    case 5: return "TRB Error";
    case 6: return "Stall";
    case 13: return "Short Packet";
    default: return "not issued";
    }
}

static void describe_controller(int index, int bus, int dev, int fn) {
    char line[96];
    int n = 0;
    uint32_t id = pci_read(bus, dev, fn, 0);
    uint32_t lo = pci_read(bus, dev, fn, 0x10);
    uint32_t hi = ((lo & 7u) == 4u) ? pci_read(bus, dev, fn, 0x14) : 0;
    uint64_t phys = ((uint64_t)hi << 32) | (lo & ~0xFu);
    const BootInfo *bi = bootinfo();
    uint32_t mmio = 0;
    int match = bi->magic == BOOTINFO_MAGIC && (bi->xhci_pci & 0x80000000u) &&
                bus == (int)((bi->xhci_pci >> 16) & 0xFF) &&
                dev == (int)((bi->xhci_pci >> 8) & 0x1F) &&
                fn == (int)(bi->xhci_pci & 7);
    if (match && bi->xhci_mmio) mmio = bi->xhci_mmio;
    else if (phys && phys < 0x100000000ULL) {
        force_uc(phys);
        mmio = (uint32_t)phys;
    } else if (phys) mmio = map_high(phys, index);

    line[n++] = 'x';
    line[n++] = 'h';
    line[n++] = 'c';
    line[n++] = 'i';
    put_dec(line, &n, (uint32_t)index);
    line[n++] = ' ';
    put_hex16(line, &n, (uint32_t)bus);
    line[n++] = ':';
    put_hex16(line, &n, (uint32_t)dev);
    line[n++] = '.';
    line[n++] = (char)('0' + (fn & 7));
    line[n++] = ' ';
    put_hex4(line, &n, id & 0xFFFF);
    line[n++] = ':';
    put_hex4(line, &n, id >> 16);
    line[n++] = ' ';
    line[n++] = 'B';
    line[n++] = 'A';
    line[n++] = 'R';
    line[n++] = ' ';
    put_hex8(line, &n, hi);
    put_hex8(line, &n, lo & ~0xFu);
    line[n] = 0;
    add_line(line);

    if (!mmio) {
        add_line("  mmio unmapped  compl: not issued");
        return;
    }
    volatile uint32_t *cap32 = (volatile uint32_t *)(uintptr_t)mmio;
    uint32_t cap0 = cap32[0];
    uint8_t caplen = (uint8_t)cap0;
    if (caplen < 0x10 || caplen > 0x40) {
        add_line("  mmio invalid  compl: not issued");
        return;
    }
    uint16_t ver = (uint16_t)(cap0 >> 16);
    volatile uint8_t *cap = (volatile uint8_t *)cap32;
    uint32_t hcs1 = *(volatile uint32_t *)(cap + 4);
    uint32_t hcs2 = *(volatile uint32_t *)(cap + 8);
    uint32_t slots = hcs1 & 0xFFu;
    uint32_t ports = (hcs1 >> 24) & 0xFFu;
    uint32_t scratch = xhci_scratchpad_count(hcs2);
    const XhciHandoff *hand = 0;
    for (int i = 0; i < xhci_handoff_count(); i++) {
        const XhciHandoff *h = xhci_handoff_at(i);
        if (h && h->bus == (uint8_t)bus && h->dev == (uint8_t)dev && h->fn == (uint8_t)fn) {
            hand = h;
            break;
        }
    }
    uint32_t before = hand ? hand->leg_before : 0;
    uint32_t before_ctl = hand ? hand->ctl_before : 0;
    uint32_t after = hand ? hand->leg_after : 0;
    uint32_t after_ctl = hand ? hand->ctl_after : 0;

    n = 0;
    line[n++] = ' ';
    line[n++] = ' ';
    line[n++] = 'H';
    line[n++] = 'C';
    line[n++] = 'I';
    line[n++] = ' ';
    put_dec(line, &n, ver >> 8);
    line[n++] = '.';
    line[n++] = "0123456789ABCDEF"[(ver >> 4) & 0xF];
    line[n++] = "0123456789ABCDEF"[ver & 0xF];
    line[n++] = ' ';
    line[n++] = 's';
    line[n++] = 'l';
    put_dec(line, &n, slots);
    line[n++] = ' ';
    line[n++] = 'p';
    put_dec(line, &n, ports);
    line[n++] = ' ';
    line[n++] = 's';
    line[n++] = 'c';
    put_dec(line, &n, scratch);
    line[n] = 0;
    add_line(line);

    if (!hand || !hand->have_leg) {
        add_line("  LEG none");
    } else {
        n = 0;
        const char *leg = "  LEG ";
        while (*leg) line[n++] = *leg++;
        put_hex8(line, &n, before);
        line[n++] = '>';
        put_hex8(line, &n, after);
        line[n++] = ' ';
        line[n++] = 'C';
        put_hex8(line, &n, before_ctl);
        line[n++] = '>';
        put_hex8(line, &n, after_ctl);
        line[n] = 0;
        add_line(line);
    }

    n = 0;
    line[n++] = ' ';
    line[n++] = ' ';
    volatile uint32_t *op = (volatile uint32_t *)(cap + caplen);
    uint32_t show = ports > 10 ? 10 : ports;
    for (uint32_t p = 0; p < show && n < 84; p++) {
        uint32_t psc = op[(0x400 / 4) + p * 4];
        line[n++] = 'p';
        put_dec(line, &n, p);
        line[n++] = 'C';
        line[n++] = (psc & 1u) ? '1' : '0';
        line[n++] = 'S';
        put_hex16(line, &n, (psc >> 10) & 0xF);
        line[n++] = ' ';
    }
    if (ports > show) {
        line[n++] = '+';
        put_dec(line, &n, ports - show);
    }
    line[n] = 0;
    add_line(line);
    n = 0;
    const char *tag = "  compl: ";
    while (*tag) line[n++] = *tag++;
    tag = compl_name(0);
    while (*tag) line[n++] = *tag++;
    line[n] = 0;
    add_line(line);
}

static void collect_xhci(void) {
    int index = 0;
    for (int bus = 0; bus < 256 && index < 3; bus++) {
        for (int dev = 0; dev < 32 && index < 3; dev++) {
            for (int fn = 0; fn < 8 && index < 3; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if (id == 0xFFFFFFFFu || id == 0) {
                    if (fn == 0) break;
                    continue;
                }
                uint32_t cls = pci_read(bus, dev, fn, 8);
                if ((uint8_t)(cls >> 24) == 0x0C && (uint8_t)(cls >> 16) == 0x03 &&
                    (uint8_t)(cls >> 8) == 0x30) {
                    describe_controller(index++, bus, dev, fn);
                }
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    if (!index) add_line("xhci none");
}

static void collect_8042(void) {
    char line[96];
    int n = 0;
    uint8_t st = inb(0x64);
    const char *prefix = "8042 st ";
    while (*prefix) line[n++] = *prefix++;
    put_hex16(line, &n, st);
    ps2_cmd(0x20);
    int cfg = ps2_read_raw(200000);
    const char *mid = " cfg ";
    while (*mid) line[n++] = *mid++;
    if (cfg < 0) line[n++] = '-';
    else put_hex16(line, &n, (uint32_t)cfg);
    ps2_cmd(0xAB);
    int ab = ps2_read_raw(200000);
    mid = " AB ";
    while (*mid) line[n++] = *mid++;
    if (ab < 0) line[n++] = '-';
    else put_hex16(line, &n, (uint32_t)ab);
    line[n] = 0;
    add_line(line);

    n = 0;
    mid = "ID";
    while (*mid) line[n++] = *mid++;
    ps2_data(0xF2);
    int idb[4];
    for (int i = 0; i < 4; i++) idb[i] = ps2_read_raw(200000);
    format_bytes(line, &n, idb, 4);
    mid = " EE";
    while (*mid) line[n++] = *mid++;
    ps2_data(0xEE);
    int echo[2];
    echo[0] = ps2_read_raw(200000);
    echo[1] = ps2_read_raw(50000);
    format_bytes(line, &n, echo, 2);
    line[n] = 0;
    add_line(line);
}

static void collect_timer(void) {
    char line[96];
    int n = 0;
    uint16_t a = pit_now();
    for (int i = 0; i < 200000; i++) pit_now();
    uint16_t b = pit_now();
    const char *p = "PIT ";
    while (*p) line[n++] = *p++;
    line[n++] = (a != b) ? 'Y' : 'N';
    p = " ticks ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, timer_ticks());
    p = " CLK ";
    while (*p) line[n++] = *p++;
    p = timer_source();
    while (*p && n < 80) line[n++] = *p++;
    p = timer_hpet_found() ? " hpet Y" : " hpet N";
    while (*p && n < 90) line[n++] = *p++;
    line[n] = 0;
    add_line(line);

    n = 0;
    p = "TSC ";
    while (*p) line[n++] = *p++;
    uint64_t t0 = tsc_now();
    int wraps = 0;
    uint16_t prev = pit_now();
    for (int i = 0; i < 4000000 && wraps < 10; i++) {
        uint16_t now = pit_now();
        if (now > prev) wraps++;
        prev = now;
    }
    uint64_t t1 = tsc_now();
    put_hex8(line, &n, (uint32_t)(t1 >> 32));
    put_hex8(line, &n, (uint32_t)t1);
    line[n++] = '-';
    put_hex8(line, &n, (uint32_t)(t0 >> 32));
    put_hex8(line, &n, (uint32_t)t0);
    p = " w";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, (uint32_t)wraps);
    line[n] = 0;
    add_line(line);
}

static void collect_memory(void) {
    const BootInfo *bi = bootinfo();
    char line[96];
    int n = 0;
    const char *p = bi->fw_vendor[0] ? bi->fw_vendor : "(no firmware name)";
    while (*p && n < 70) line[n++] = *p++;
    line[n++] = ' ';
    put_hex8(line, &n, bi->fw_revision);
    line[n] = 0;
    add_line(line);

    n = 0;
    p = "GOP ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, bi->width);
    line[n++] = 'x';
    put_dec(line, &n, bi->height);
    p = " mode ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, bi->gop_mode);
    line[n] = 0;
    add_line(line);

    n = 0;
    p = "MEM ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, bi->usable_regions);
    p = " big ";
    while (*p) line[n++] = *p++;
    put_hex8(line, &n, bi->largest_base);
    line[n++] = '+';
    put_dec(line, &n, bi->largest_pages / 256u);
    line[n++] = 'M';
    p = " hi ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, bi->high_pages / 256u);
    line[n++] = 'M';
    line[n] = 0;
    add_line(line);

    n = 0;
    p = "overlap ";
    while (*p) line[n++] = *p++;
    if (!bi->fw_vendor[0] && !bi->usable_regions && !bi->gop_mode) {
        p = "no UEFI map";
        while (*p) line[n++] = *p++;
        line[n] = 0;
        add_line(line);
        return;
    }
    if (!bi->overlap_mask) {
        p = "none";
        while (*p) line[n++] = *p++;
    } else {
        for (int i = 0; i < 10 && n < 88; i++) {
            if ((bi->overlap_mask & (1u << i)) == 0) continue;
            uint32_t addr = watch_addr[i];
            if (i == 7) addr = bi->image_arena;
            if (i == 9) addr = bi->xhci_heap ? bi->xhci_heap : watch_addr[i];
            if (!addr) continue;
            line[n++] = ' ';
            put_hex8(line, &n, addr);
        }
    }
    line[n] = 0;
    add_line(line);
}

static void collect_steps(void) {
    for (int i = 0; i < acpi_step_count(); i++) add_line(acpi_step_line(i));
    for (int i = 0; i < input_step_count(); i++) add_line(input_step_line(i));
    add_line("1 IRQ   2 self-test AA   3 no reset");
}

static void collect(void) {
    fixed_count = 0;
    collect_memory();
    collect_timer();
    collect_steps();
    collect_xhci();
    for (int i = 0; i < xhci_usb_count(); i++) add_line(xhci_usb_line(i));
    probed = 1;
}

static void draw_log_and_counts(int row) {
    char line[96];
    int shown = input_ps2_log_count();
    if (shown > 24) shown = 24;
    int n = 0;
    const char *p = "LOG";
    while (*p) line[n++] = *p++;
    for (int i = shown - 1; i >= 0 && n < 86; i--) {
        uint8_t st, data;
        input_ps2_log_get(i, &st, &data);
        line[n++] = ' ';
        put_hex16(line, &n, st);
        line[n++] = ':';
        put_hex16(line, &n, data);
    }
    if (!shown) {
        p = " (none)";
        while (*p) line[n++] = *p++;
    }
    line[n] = 0;
    if (row < 44) gfx_text(4, 40 + row * 16, line, COL_WHITE, COL_BLACK);

    n = 0;
    p = "COUNT kbd ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, input_ps2_kbd_bytes());
    p = " aux ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, input_ps2_aux_bytes());
    p = " usb-kbd ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, input_usb_kbd_packets());
    p = " usb-mouse ";
    while (*p) line[n++] = *p++;
    put_dec(line, &n, input_usb_mouse_packets());
    line[n] = 0;
    if (row + 1 < 44) gfx_text(4, 40 + (row + 1) * 16, line, COL_YELLOW, COL_BLACK);
}

void diag_draw_build_id(int x, int y, int scale) {
    char line[80];
    int n = 0;
    const char *p = "BUILD ";
    while (*p) line[n++] = *p++;
    p = GOS_BUILD_HASH;
    while (*p && n < 70) line[n++] = *p++;
    line[n++] = ' ';
    p = GOS_BUILD_UTC;
    while (*p && n < 78) line[n++] = *p++;
    line[n] = 0;
    if (scale < 1) scale = 1;
    int tw = n * 8 * scale;
    gfx_fill(x - 4, y - 2, tw + 8, 16 * scale + 4, COL_BLACK);
    gfx_text_scaled(x, y, line, COL_YELLOW, scale);
}

void diag_run(void) {
    serial_write("DIAG: SHOW ");
    serial_write(GOS_BUILD_HASH);
    serial_write(" ");
    serial_write(GOS_BUILD_UTC);
    serial_write("\n");
    if (!probed) collect();
    int logged = 0;
    for (;;) {
        gfx_reset_clip();
        gfx_clear(COL_BLACK);
        diag_draw_build_id(8, 4, 2);
        int row = 0;
        int limit = (gfx_height() - 56) / 16;
        if (limit > 40) limit = 40;
        for (; row < fixed_count && row < limit; row++)
            gfx_text(4, 40 + row * 16, fixed_line[row], COL_WHITE, COL_BLACK);
        if (row + 1 < limit) draw_log_and_counts(row);
        int seen = input_ps2_log_count();
        while (logged < seen) {
            uint8_t st, data;
            input_ps2_log_get(seen - 1 - logged, &st, &data);
            serial_write("DIAG byte ");
            serial_hex32(((uint32_t)st << 8) | data);
            serial_write("\n");
            logged++;
        }
        gfx_present();
        input_poll();
        KeyEvent key;
        if (!input_key(&key)) continue;
        if (key.code == '1' || key.code == '2' || key.code == '3') {
            int flags = 0;
            if (key.code == '1') flags = KBD_IRQ;
            if (key.code == '2') flags = KBD_AA;
            if (key.code == '3') flags = KBD_NO_RESET;
            input_kbd_reinit(flags);
            collect();
            logged = 0;
            continue;
        }
        break;
    }
    serial_write("DIAG: EXIT\n");
}
