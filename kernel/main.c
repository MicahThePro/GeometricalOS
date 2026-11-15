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

static void boot_console(const BootInfo *bi, const AtaDisk *disk,
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
    gfx_text(40, gfx_height() - 52,
             "Press ENTER to continue, or wait for automatic startup.",
             COL_DKGRAY, COL_BLACK);
    gfx_present();

    uint32_t start = timer_ticks();
    while ((uint32_t)(timer_ticks() - start) < 150u) {
        input_poll();
        int skip = 0;
        KeyEvent key;
        while (input_key(&key))
            if (key.code == 10 || key.code == 13) skip = 1;
        MouseEvent mouse;
        while (input_mouse(&mouse))
            if (mouse.type == MOUSE_LDOWN) skip = 1;
        if (skip) break;
    }
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
    serial_write("\nGeometricalOS kernel\n");
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
    input_init();
    if (bi->ext_mem_kb && bi->ext_mem_kb < 7000) {
        boot_console(bi, 0, BOOT_NOT_CHECKED, BOOT_NOT_CHECKED,
                     "Boot halted: insufficient extended memory");
        halt_screen("GeometricalOS", "This computer needs more memory.", "Give it at least 32 MB of RAM.");
    }
    if (!bi->from_hdd) {
        boot_console(bi, 0, BOOT_NOT_CHECKED, BOOT_NOT_CHECKED,
                     "Starting the CD installer");
        run_setup();
        reboot();
    }
    AtaDisk disk;
    int disk_result = ata_probe(&disk);
    int fs_result = fs_mount();
    boot_console(bi, &disk, disk_result, fs_result,
                 fs_result == 0 ? "Starting the installed system" : "Boot halted: disk is not installed");
    if (fs_result != 0) {
        serial_write("FS: mount failed\n");
        halt_screen("GeometricalOS", "This hard disk is not installed.", "Boot the GeometricalOS CD and run Setup.");
    }
    settings_load();
    splash_screen();
    login_screen();
    desktop_init();
    serial_write("DESKTOP: READY\n");
    gui_run();
}
