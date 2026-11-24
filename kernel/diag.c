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
#include "i2chid.h"

/* Fixed addresses the kernel uses. Bit i of overlap_mask means address i
 * was not inside a usable UEFI region. */
static const uint32_t watch_addr[] = {
    0x6000u, 0x7000u, 0x7C00u, 0x10000u, 0x12000u,
    0x80000u, 0x90000u, 0u, 0x1800000u, 0x1100000u
};

#define LINE_MAX 200

#define FIXED_MAX 16
static char (*fixed_line)[LINE_MAX];
static int fixed_count;
static int probed;
static int shown;

static void add_line(const char *text) {
    serial_write(text);
    serial_write("\n");
    if (!fixed_line || fixed_count >= FIXED_MAX) return;
    int i = 0;
    for (; text[i] && i < LINE_MAX - 1; i++) fixed_line[fixed_count][i] = text[i];
    fixed_line[fixed_count][i] = 0;
    fixed_count++;
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

/* Two lines per controller: identity, version, legacy handoff; then the
 * root ports and the command ring state. */
static void describe_controller(int index, int bus, int dev, int fn) {
    char text[LINE_MAX];
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
        xhci_force_uc((uint32_t)phys);
        mmio = (uint32_t)phys;
    } else if (phys) mmio = map_high(phys, index);

    n = kfmt(text, LINE_MAX, "X%u %2x:%2x.%u %4x:%4x BAR ", (uint32_t)index,
             (uint32_t)bus, (uint32_t)dev, (uint32_t)(fn & 7), id & 0xFFFF, id >> 16);
    if (hi) n += kfmt(text + n, LINE_MAX - n, "%8x", hi);
    n += kfmt(text + n, LINE_MAX - n, "%8x", lo & ~0xFu);

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)mmio;
    uint8_t caplen = mmio ? cap[0] : 0;
    if (!mmio || caplen < 0x10 || caplen > 0x40) {
        kfmt(text + n, LINE_MAX - n, mmio ? " mmio invalid" : " mmio unmapped");
        add_line(text);
        add_line(xhci_cmd_line(bus, dev, fn));
        return;
    }
    uint32_t cap0 = *(volatile uint32_t *)cap;
    uint16_t ver = (uint16_t)(cap0 >> 16);
    uint32_t hcs1 = *(volatile uint32_t *)(cap + 4);
    uint32_t hcs2 = *(volatile uint32_t *)(cap + 8);
    uint32_t ports = (hcs1 >> 24) & 0xFFu;
    n += kfmt(text + n, LINE_MAX - n, " v%4x sl%u p%u sc%u", ver, hcs1 & 0xFFu, ports,
              xhci_scratchpad_count(hcs2));
    const XhciHandoff *hand = 0;
    for (int i = 0; i < xhci_handoff_count(); i++) {
        const XhciHandoff *h = xhci_handoff_at(i);
        if (h && h->bus == (uint8_t)bus && h->dev == (uint8_t)dev && h->fn == (uint8_t)fn) {
            hand = h;
            break;
        }
    }
    if (!hand || !hand->have_leg) {
        kfmt(text + n, LINE_MAX - n, " LEG -");
    } else {
        kfmt(text + n, LINE_MAX - n, " LEG %8x>%8x C%8x>%8x", hand->leg_before, hand->leg_after,
             hand->ctl_before, hand->ctl_after);
    }
    add_line(text);

    n = kfmt(text, LINE_MAX, " ");
    volatile uint32_t *op = (volatile uint32_t *)(cap + caplen);
    uint32_t show = ports > 12 ? 12 : ports;
    for (uint32_t p = 0; p < show; p++) {
        uint32_t psc = op[(0x400 / 4) + p * 4];
        n += kfmt(text + n, LINE_MAX - n, " %u%c%c%u", p + 1, (psc & 1u) ? 'C' : '.',
                  (psc & 2u) ? 'E' : '.', (psc >> 10) & 0xF);
    }
    if (ports > show) n += kfmt(text + n, LINE_MAX - n, " +%u", ports - show);
    kfmt(text + n, LINE_MAX - n, " %s", xhci_cmd_line(bus, dev, fn));
    add_line(text);
}

static void collect_xhci(void) {
    int index = 0;
    for (int bus = 0; bus < 256 && index < 4; bus++) {
        for (int dev = 0; dev < 32 && index < 4; dev++) {
            for (int fn = 0; fn < 8 && index < 4; fn++) {
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
    if (!index) add_line("X none");
}

/* Firmware, video, memory, and overlap on one line; clock; ACPI; PS/2. */
static void collect_system(void) {
    const BootInfo *bi = bootinfo();
    char text[LINE_MAX];
    char fw[24];
    int f = 0;
    for (const char *p = bi->fw_vendor; *p && f < 23; p++) fw[f++] = *p;
    fw[f] = 0;
    int n = kfmt(text, LINE_MAX, "%s %8x GOP %ux%u m%u MEM %ur %8x+%uM hi%uM OVL",
                 f ? fw : "noFW", bi->fw_revision, bi->width, bi->height, bi->gop_mode,
                 bi->usable_regions, bi->largest_base, bi->largest_pages / 256u,
                 bi->high_pages / 256u);
    if (!bi->fw_vendor[0] && !bi->usable_regions && !bi->gop_mode) {
        kfmt(text + n, LINE_MAX - n, " nomap");
    } else if (!bi->overlap_mask) {
        kfmt(text + n, LINE_MAX - n, " none");
    } else {
        for (int i = 0; i < 10; i++) {
            if ((bi->overlap_mask & (1u << i)) == 0) continue;
            uint32_t addr = watch_addr[i];
            if (i == 7) addr = bi->image_arena;
            if (i == 9) addr = bi->xhci_heap ? bi->xhci_heap : watch_addr[i];
            if (!addr) continue;
            n += kfmt(text + n, LINE_MAX - n, " %8x", addr);
        }
    }
    add_line(text);

    n = kfmt(text, LINE_MAX, "CLK %s%s%s ACPI", timer_source(),
             timer_hpet_found() ? " hpet Y" : " hpet N", timer_pit_alive() ? " pit Y" : " pit N");
    for (int i = 0; i < acpi_step_count(); i++)
        n += kfmt(text + n, LINE_MAX - n, "%s%s", i ? "; " : " ", acpi_step_line(i));
    add_line(text);

    n = kfmt(text, LINE_MAX, "PS2%s", input_ps2_present() ? " present" : " absent");
    for (int i = 0; i < input_step_count(); i++)
        n += kfmt(text + n, LINE_MAX - n, "%s%s", i ? "; " : " ", input_step_line(i));
    add_line(text);
}

static void collect(void) __attribute__((noinline));
static void collect(void) {
    fixed_count = 0;
    if (!fixed_line) fixed_line = xhci_heap_alloc(FIXED_MAX * LINE_MAX);
    collect_system();
    collect_xhci();
    for (int i = 0; i < xhci_usb_count(); i++) {
        serial_write(xhci_usb_line(i));
        serial_write("\n");
    }
    for (int i = 0; i < i2chid_line_count(); i++) {
        serial_write(i2chid_line(i));
        serial_write("\n");
    }
    probed = 1;
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

int diag_was_shown(void) { return shown; }

/* Long lines wrap with a two-space indent. Returns the next row. */
static int draw_wrapped(int row, int limit, const char *s, uint32_t color) {
    int cols = (gfx_width() - 8) / 8;
    if (cols < 40) cols = 40;
    char chunk[LINE_MAX];
    int first = 1;
    while (*s && row < limit) {
        int indent = first ? 0 : 2;
        int take = cols - indent;
        int n = 0;
        for (int i = 0; i < indent; i++) chunk[n++] = ' ';
        while (*s && take-- > 0 && n < LINE_MAX - 1) chunk[n++] = *s++;
        chunk[n] = 0;
        gfx_text(4, 40 + row * 16, chunk, color, COL_BLACK);
        row++;
        first = 0;
    }
    return row;
}

static char typed[72];
static int typed_n;

static void type_char(int code) {
    char c = 0;
    if (code >= 32 && code < 127) c = (char)code;
    else if (code == '\b') {
        if (typed_n) typed_n--;
        typed[typed_n] = 0;
        return;
    } else if (code == '\n' || code == '\r') c = '|';
    else if (code >= KEY_UP && code <= KEY_RIGHT) c = "^v<>"[code - KEY_UP];
    else if (code >= KEY_F1 && code <= KEY_F12) c = '#';
    else return;
    if (typed_n >= (int)sizeof(typed) - 1) {
        for (int i = 1; i < typed_n; i++) typed[i - 1] = typed[i];
        typed_n--;
    }
    typed[typed_n++] = c;
    typed[typed_n] = 0;
}

void diag_run(void) {
    serial_write("DIAG: SHOW ");
    serial_write(GOS_BUILD_HASH);
    serial_write(" ");
    serial_write(GOS_BUILD_UTC);
    serial_write("\n");
    shown = 1;
    if (!probed) collect();
    int bw = 120, bh = 28;
    for (;;) {
        gfx_reset_clip();
        gfx_clear(COL_BLACK);
        diag_draw_build_id(8, 4, 2);
        int bx0 = gfx_width() - bw - 12, by0 = 6;
        gfx_button(bx0, by0, bw, bh, "Continue", mouse_buttons & 1);
        int limit = (gfx_height() - 44) / 16;
        int row = 0;
        for (int i = 0; i < fixed_count && row < limit; i++)
            row = draw_wrapped(row, limit, fixed_line[i], i < 3 ? COL_WHITE : COL_GRAY);
        for (int i = 0; i < xhci_usb_count() && row < limit; i++)
            row = draw_wrapped(row, limit, xhci_usb_line(i), COL_CYAN);
        if (!xhci_usb_count()) row = draw_wrapped(row, limit, "USB no devices", COL_CYAN);
        for (int i = 0; i < i2chid_line_count() && row < limit; i++)
            row = draw_wrapped(row, limit, i2chid_line(i), COL_LTGREEN);

        char text[LINE_MAX];
        kfmt(text, LINE_MAX,
             "COUNT usb-kbd %u usb-mouse %u i2c %u keys %u ptr %u hiderr %u/%2x ps2 %u/%u xy %u,%u b%u",
             xhci_kbd_reports(), xhci_mouse_reports(), i2chid_packets(), input_usb_kbd_packets(),
             input_usb_mouse_packets(), xhci_hid_errors(), xhci_hid_last_error(),
             input_ps2_kbd_bytes(), input_ps2_aux_bytes(), (uint32_t)mouse_x, (uint32_t)mouse_y,
             (uint32_t)mouse_buttons);
        row = draw_wrapped(row, limit, text, COL_YELLOW);
        kfmt(text, LINE_MAX, "TYPE> %s_   (Esc or click Continue)", typed);
        draw_wrapped(row, limit, text, COL_WHITE);

        gfx_present();
        cursor_invalidate();
        cursor_draw(mouse_x, mouse_y);
        input_poll();
        int done = 0;
        KeyEvent key;
        while (input_key(&key)) {
            serial_write("DIAG key ");
            serial_hex32((uint32_t)key.code);
            serial_write("\n");
            if (key.code == 27) done = 1;
            else type_char(key.code);
        }
        MouseEvent m;
        while (input_mouse(&m)) {
            if (m.type == MOUSE_LUP && m.x >= bx0 && m.x < bx0 + bw && m.y >= by0 && m.y < by0 + bh)
                done = 1;
        }
        if (done) break;
    }
    serial_write("DIAG: EXIT\n");
}
