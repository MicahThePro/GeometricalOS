#include "apps.h"
#include "gui.h"
#include "bootinfo.h"
#include "ata.h"
#include "gfx.h"
#include "input.h"
#include "fs.h"
#include "serial.h"
#include "io.h"
#include "sound.h"
#include "settings.h"
#include "recovery.h"
#include "nvme.h"
#include "gfx.h"
#include "diag.h"
#include "buildid.h"
#include "memmap.h"
#include "xhci.h"
#include "acpi.h"

#define BOOT_NOT_CHECKED (-100)

static int append_dec(char *buf, int pos, uint32_t value) {
    char digits[10];
    int n = 0;
    if (!value) digits[n++] = '0';
    while (value && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (n) buf[pos++] = digits[--n];
    buf[pos] = 0;
    return pos;
}

static int append_hex32(char *buf, int pos, uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        buf[pos++] = hex[(value >> shift) & 0x0F];
    buf[pos] = 0;
    return pos;
}

static int append_hex8(char *buf, int pos, uint8_t value) {
    static const char hex[] = "0123456789ABCDEF";
    buf[pos++] = '0';
    buf[pos++] = 'x';
    buf[pos++] = hex[value >> 4];
    buf[pos++] = hex[value & 0x0F];
    buf[pos] = 0;
    return pos;
}

static void boot_line(int row, const char *label, const char *value, uint32_t color) {
    int y = 94 + row * 28;
    gfx_text(40, y, label, COL_LTGREEN, COL_BLACK);
    gfx_text(240, y, value, color, COL_BLACK);
}

static int boot_console(const BootInfo *bi, const AtaDisk *disk,
                        int disk_result, int fs_result, const char *next) {
    char value[80];
    int n;

    gfx_reset_clip();
    gfx_clear(COL_BLACK);
    gfx_rect(24, 24, gfx_width() - 48, gfx_height() - 48, COL_GREEN);
    gfx_fill(26, 26, gfx_width() - 52, 28, COL_NAVY);
    gfx_text(40, 32, "GEOMETRICALOS BOOT CONSOLE", COL_WHITE, COL_NAVY);
    gfx_text(40, 66, "BIOS handoff and detected hardware", COL_LTGREEN, COL_BLACK);

    n = 0;
    strcpy(value, bi->from_hdd ? "Hard disk / MBR, BIOS DL=" : "CD-ROM / El Torito, BIOS DL=");
    n = (int)strlen(value);
    n = append_hex8(value, n, (uint8_t)bi->boot_drive);
    strcpy(value + n, bi->from_hdd ? " (HDD boot)" : " (installer boot)");
    boot_line(0, "BOOT SOURCE", value, COL_WHITE);

    if (bi->ext_mem_kb) {
        n = append_dec(value, 0, bi->ext_mem_kb);
        strcpy(value + n, " KB above 1 MiB");
    } else {
        strcpy(value, "Not reported by BIOS");
    }
    boot_line(1, "BIOS MEMORY", value, bi->ext_mem_kb ? COL_WHITE : COL_YELLOW);

    n = append_dec(value, 0, bi->width);
    strcpy(value + n, " x ");
    n += 3;
    n = append_dec(value, n, bi->height);
    strcpy(value + n, " x ");
    n += 3;
    n = append_dec(value, n, bi->bpp);
    strcpy(value + n, " bpp");
    boot_line(2, "VIDEO MODE", value, COL_WHITE);

    n = append_hex32(value, 0, bi->lfb);
    strcpy(value + n, " linear framebuffer");
    boot_line(3, "FRAMEBUFFER", value, COL_WHITE);

    if (disk_result == BOOT_NOT_CHECKED) {
        strcpy(value, "Not probed on CD boot");
    } else if (disk_result == 0 && disk && disk->present) {
        strcpy(value, disk->model[0] ? disk->model : "Unknown ATA model");
    } else if (disk_result == -2) {
        strcpy(value, "ATAPI device (not a hard disk)");
    } else {
        strcpy(value, "No ATA primary-master disk detected");
    }
    boot_line(4, "IDE PRIMARY MASTER", value,
              disk_result == 0 ? COL_WHITE : COL_YELLOW);

    if (disk_result == 0 && disk && disk->present) {
        n = append_dec(value, 0, disk->sectors / 2048u);
        strcpy(value + n, " MiB (");
        n += 6;
        n = append_dec(value, n, disk->sectors);
        strcpy(value + n, " sectors)");
    } else {
        strcpy(value, disk_result == BOOT_NOT_CHECKED ? "Not checked" : "Unavailable");
    }
    boot_line(5, "DISK CAPACITY", value,
              disk_result == 0 ? COL_WHITE : COL_YELLOW);

    if (fs_result == BOOT_NOT_CHECKED)
        strcpy(value, "Not mounted; starting installer");
    else if (fs_result == 0)
        strcpy(value, "GeoFS mounted from the IDE disk");
    else
        strcpy(value, "Mount failed; installed system unavailable");
    boot_line(6, "FILESYSTEM", value, fs_result == 0 ? COL_WHITE : COL_YELLOW);

    boot_line(7, "NEXT", next, COL_WHITE);
    gfx_text(40, gfx_height() - 72,
             "F8 opens recovery. D opens the hardware DIAG.",
             COL_DKGRAY, COL_BLACK);
    gfx_text(40, gfx_height() - 52,
             "Startup continues in 5 seconds.",
             COL_DKGRAY, COL_BLACK);
    gfx_present();

    /* Defer VMM display changes until the next screen can be painted. */
    uint32_t start = timer_ticks();
    int recovery = 0;
    int open_diag = 0;
    while ((uint32_t)(timer_ticks() - start) < 500u) {
        input_poll();
        KeyEvent key;
        while (input_key(&key)) {
            if (key.code == KEY_F8) recovery = 1;
            if (key.code == 'd' || key.code == 'D') open_diag = 1;
        }
        if (open_diag) break;
    }
    if (open_diag) diag_run();
    MouseEvent mouse;
    while (input_mouse(&mouse)) {}
    return recovery;
}

static void halt_screen(const char *title, const char *a, const char *b) {
    gfx_reset_clip();
    gfx_clear(COL_SETUPBLUE);
    int w = 460, h = 180;
    int x = (gfx_width() - w) / 2;
    int y = (gfx_height() - h) / 2;
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, 18, COL_NAVY);
    gfx_text(x + 8, y + 4, title, COL_WHITE, COL_NAVY);
    gfx_text(x + 16, y + 40, a, COL_BLACK, COL_GRAY);
    if (b) gfx_text(x + 16, y + 60, b, COL_BLACK, COL_GRAY);
    gfx_present();
    for (;;) input_poll();
}

void kmain(void) {
    cli_if();
    serial_init();
    serial_write("\nGeometricalOS kernel\nBUILD ");
    serial_write(GOS_BUILD_HASH);
    serial_write(" ");
    serial_write(GOS_BUILD_UTC);
    serial_write("\n");
    const BootInfo *bi = bootinfo();
    serial_write("magic ");
    serial_hex32(bi->magic);
    serial_write(" ");
    serial_dec(bi->width);
    serial_write("x");
    serial_dec(bi->height);
    serial_write("x");
    serial_dec(bi->bpp);
    serial_write(" lfb ");
    serial_hex32(bi->lfb);
    serial_write(" memKB ");
    serial_dec(bi->ext_mem_kb);
    serial_write(bi->from_hdd ? " hdd drive " : " cd drive ");
    serial_hex32(bi->boot_drive);
    serial_write("\n");

    if (bi->magic != BOOTINFO_MAGIC || gfx_init() != 0) {
        serial_write("graphics init failed\n");
        for (;;) cli_if();
    }
    /* Calibrate time, then take every xHCI controller away from firmware
     * SMM before anything reads port 0x60 or 0x64. Enumeration stays off. */
    timer_calibrate();
    mem_selftest();
    xhci_handoff_all();
    acpi_bringup();
    input_init();
    if (bi->ext_mem_kb && bi->ext_mem_kb < 7000) {
        AtaDisk none;
        memset(&none, 0, sizeof(none));
        boot_console(bi, &none, BOOT_NOT_CHECKED, BOOT_NOT_CHECKED,
                     "Boot halted: insufficient extended memory");
        recovery_boot_menu(bi, &none, BOOT_NOT_CHECKED, -1);
        halt_screen("GeometricalOS", "This computer needs more memory.", "Give it at least 32 MB of RAM.");
    }
    if (!bi->from_hdd) {
        uint64_t installed_start = 0;
        uint32_t installed_sectors = 0;
        if (nvme_find_geofs(&installed_start, &installed_sectors) &&
            !disk_use_nvme(installed_start, installed_sectors) && !fs_mount()) {
            gfx_clear(COL_SETUPBLUE);
            gfx_text(48, 48, "Installed GeometricalOS", COL_WHITE, COL_SETUPBLUE);
            gfx_text(48, 72, "Esc opens Setup. Any other key waits.", COL_YELLOW, COL_SETUPBLUE);
            gfx_present();
            uint32_t started = timer_ticks();
            int setup_instead = 0;
            while (timer_ticks() - started < 200) {
                input_poll();
                KeyEvent key;
                while (input_key(&key)) if (key.code == 27) setup_instead = 1;
                if (setup_instead) break;
            }
            if (!setup_instead) {
                settings_load();
                splash_screen();
                login_screen();
                desktop_init();
                serial_write("DESKTOP: READY\n");
                gui_run();
                reboot();
            }
            disk_use_nvme(0, 0);
        }
        int recovery_requested = boot_console(
            bi, 0, BOOT_NOT_CHECKED, BOOT_NOT_CHECKED, "Starting the CD installer");
        if (recovery_requested) {
            AtaDisk none;
            memset(&none, 0, sizeof(none));
            recovery_boot_menu(bi, &none, BOOT_NOT_CHECKED, -1);
        }
        int setup_rc = run_setup();
        if (setup_rc != 2 && setup_rc != 3) reboot();
        serial_write("LIVE: desktop\n");
        settings_load();
        splash_screen();
        login_screen();
        desktop_init();
        serial_write("DESKTOP: READY\n");
        gui_run();
        reboot();
    }
    AtaDisk disk;
    int disk_result = ata_probe(&disk);
    int fs_result = fs_mount();
    int recovery_requested = boot_console(
        bi, &disk, disk_result, fs_result,
        fs_result == 0 ? "Starting the installed system" : "Boot halted: disk is not installed");
    if (fs_result != 0) {
        serial_write("FS: mount failed\n");
        recovery_boot_menu(bi, &disk, disk_result, fs_result);
        halt_screen("GeometricalOS", "This hard disk is not installed.", "Boot the GeometricalOS CD and run Setup.");
    }
    settings_load();
    if (recovery_requested && recovery_boot_menu(bi, &disk, disk_result, fs_result))
        settings_enter_safe_mode();
    splash_screen();
    login_screen();
    desktop_init();
    serial_write("DESKTOP: READY\n");
    gui_run();
}
