#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "sound.h"
#include "serial.h"

static uint8_t *find_sig(uint8_t *start, uint32_t len, const char *sig, int n) {
    for (uint32_t i = 0; i + (uint32_t)n < len; i++) {
        int ok = 1;
        for (int k = 0; k < n; k++) if (start[i + k] != (uint8_t)sig[k]) { ok = 0; break; }
        if (ok) return start + i;
    }
    return 0;
}

static void try_acpi(void) {
    uint8_t *rsdp = 0;
    uint16_t ebda_seg = 0;
    __asm__ volatile ("movw 0x40E, %0" : "=r"(ebda_seg));
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000)
        rsdp = find_sig((uint8_t *)ebda, 1024, "RSD PTR ", 8);
    if (!rsdp) rsdp = find_sig((uint8_t *)0xE0000, 0x20000, "RSD PTR ", 8);
    if (!rsdp) return;
    uint32_t rsdt_addr = *(uint32_t *)(rsdp + 16);
    if (rsdt_addr < 0x1000) return;
    uint8_t *rsdt = (uint8_t *)rsdt_addr;
    if (rsdt[0] != 'R' || rsdt[1] != 'S' || rsdt[2] != 'D' || rsdt[3] != 'T') return;
    uint32_t len = *(uint32_t *)(rsdt + 4);
    if (len < 36 || len > 4096) return;
    uint8_t *fadt = 0;
    for (uint32_t off = 36; off + 4 <= len; off += 4) {
        uint32_t p = *(uint32_t *)(rsdt + off);
        uint8_t *t = (uint8_t *)p;
        if (t[0] == 'F' && t[1] == 'A' && t[2] == 'C' && t[3] == 'P') { fadt = t; break; }
    }
    if (!fadt) return;
    uint32_t dsdt_addr = *(uint32_t *)(fadt + 40);
    uint32_t smi_cmd = *(uint32_t *)(fadt + 48);
    uint8_t acpi_enable = fadt[52];
    uint32_t pm1a_cnt = *(uint32_t *)(fadt + 64);
    uint32_t pm1b_cnt = *(uint32_t *)(fadt + 68);
    if (!dsdt_addr || !pm1a_cnt) return;
    uint8_t *dsdt = (uint8_t *)dsdt_addr;
    uint32_t dlen = *(uint32_t *)(dsdt + 4);
    if (dlen < 36 || dlen > 256 * 1024) return;
    if (smi_cmd && acpi_enable) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (int i = 0; i < 100000; i++) {
            if (inw((uint16_t)pm1a_cnt) & 1) break;
        }
    }
    uint8_t *s5 = find_sig(dsdt, dlen, "_S5_", 4);
    if (!s5) return;
    uint8_t *p = s5 + 4;
    if (*p != 0x12) return;
    p++;
    p += ((*p & 0xC0) >> 6) + 1;
    if (*p == 0x0A) p++;
    uint16_t slp_a = (uint16_t)(*p << 10);
    p++;
    if (*p == 0x0A) p++;
    uint16_t slp_b = (uint16_t)(*p << 10);
    slp_a |= (1 << 13);
    slp_b |= (1 << 13);
    serial_write("ACPI: power off\n");
    outw((uint16_t)pm1a_cnt, slp_a);
    if (pm1b_cnt) outw((uint16_t)pm1b_cnt, slp_b);
}

static void try_poweroff(void) {
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    try_acpi();
    outw(0x4004, (uint16_t)((5 << 10) | (1 << 13)));
    outw(0x4004, (uint16_t)(1 << 13));
}

void reboot(void) {
    serial_write("REBOOT\n");
    for (int i = 0; i < 200000; i++) {
        if (!(inb(0x64) & 0x02)) break;
    }
    outb(0x64, 0xFE);
    for (;;) cli_if();
}

static void safe_message(void) {
    gfx_reset_clip();
    gfx_clear(COL_BLACK);
    const char *l1 = "It is now safe to turn off your computer.";
    int y = gfx_height() / 2 - 24;
    gfx_text((gfx_width() - gfx_text_width(l1)) / 2, y, l1, COL_WHITE, COL_BLACK);
    const char *l2 = "You can close the VirtualBox window.";
    gfx_text((gfx_width() - gfx_text_width(l2)) / 2, y + 24, l2, COL_LTBLUE, COL_BLACK);
    const char *l3 = "Press ENTER to try powering off.";
    gfx_text((gfx_width() - gfx_text_width(l3)) / 2, y + 48, l3, COL_GRAY, COL_BLACK);
    gfx_present();
    cursor_invalidate();
}

void session_shutdown(void) {
    serial_write("SHUTDOWN\n");
    sound_shutdown();
    safe_message();
    for (;;) {
        input_poll();
        KeyEvent k;
        if (input_key(&k) && (k.code == 10 || k.code == 13)) {
            try_poweroff();
            safe_message();
        }
    }
}

void session_restart(void) {
    reboot();
}
