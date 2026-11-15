#include "apps.h"
#include "recovery.h"
#include "gui.h"
#include "gfx.h"
#include "bootinfo.h"
#include "ata.h"
#include "fs.h"
#include "input.h"
#include "settings.h"
#include "wallpaper.h"
#include "serial.h"
#include "io.h"

enum { BOOT_NORMAL, BOOT_SAFE };

static Window *win;
static char status[64];
static AtaDisk info_disk;
static int info_disk_result;

static int append_dec(char *out, int pos, uint32_t value) {
    char digits[12];
    int n = 0;
    if (!value) digits[n++] = '0';
    while (value && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (n) out[pos++] = digits[--n];
    out[pos] = 0;
    return pos;
}

static void recovery_details(const BootInfo *bi, const AtaDisk *disk,
                             int disk_result, int fs_result) {
    char row[88];
    gfx_fill(0, 0, gfx_width(), gfx_height(), COL_SETUPBLUE);
    int x = 36, y = 30;
    gfx_raised(x, y, gfx_width() - 72, gfx_height() - 60, COL_GRAY);
    gfx_fill(x + 3, y + 3, gfx_width() - 78, 20, COL_NAVY);
    gfx_text(x + 10, y + 5, "GeometricalOS Recovery Diagnostics", COL_WHITE, COL_NAVY);
    gfx_text(x + 16, y + 36, "BIOS boot source:", COL_BLACK, COL_GRAY);
    gfx_text(x + 176, y + 36, bi->from_hdd ? "Hard disk" : "CD/DVD", COL_NAVY, COL_GRAY);
    int p = (int)strlen("Video mode: ");
    strcpy(row, "Video mode: ");
    p = append_dec(row, p, bi->width);
    strcpy(row + p, " x "); p += 3;
    p = append_dec(row, p, bi->height);
    strcpy(row + p, " x "); p += 3;
    p = append_dec(row, p, bi->bpp);
    strcpy(row + p, " bpp");
    gfx_text(x + 16, y + 56, row, COL_BLACK, COL_GRAY);
    strcpy(row, "BIOS extended memory: ");
    if (bi->ext_mem_kb) {
        p = append_dec(row, (int)strlen(row), bi->ext_mem_kb);
        strcpy(row + p, " KB");
    } else {
        strcpy(row + strlen(row), "not reported");
    }
    gfx_text(x + 16, y + 76, row, COL_BLACK, COL_GRAY);
    gfx_text(x + 16, y + 96, "Primary IDE disk:", COL_BLACK, COL_GRAY);
    gfx_text(x + 176, y + 96,
             disk_result == 0 && disk->present ? disk->model : "not detected",
             disk_result == 0 ? COL_NAVY : COL_MAROON, COL_GRAY);
    gfx_text(x + 16, y + 116, "Filesystem:", COL_BLACK, COL_GRAY);
    gfx_text(x + 176, y + 116, fs_result == 0 ? "GeoFS mounted" : "not mounted",
             fs_result == 0 ? COL_NAVY : COL_MAROON, COL_GRAY);
    gfx_text(x + 16, y + 148, "Press any key to return.", COL_BLACK, COL_GRAY);
    gfx_present();
    input_poll();
    KeyEvent k;
    MouseEvent m;
    for (;;) {
        input_poll();
        if (input_key(&k) || input_mouse(&m)) break;
    }
}

int recovery_boot_menu(const BootInfo *bi, const AtaDisk *disk,
                       int disk_result, int fs_result) {
    static const char *normal_items[] = {
        "Start GeometricalOS normally",
        "Start in Safe Mode",
        "Restore last good settings",
        "Reset all settings to defaults",
        "View diagnostics",
        "Restart computer"
    };
    static const char *fail_items[] = {
        "View diagnostics",
        "Restart computer"
    };
    int selected = 0;
    for (;;) {
        int count = fs_result == 0 ? 6 : 2;
        const char **items = fs_result == 0 ? normal_items : fail_items;
        gfx_fill(0, 0, gfx_width(), gfx_height(), COL_SETUPBLUE);
        int x = (gfx_width() - 480) / 2;
        int y = (gfx_height() - 350) / 2;
        if (x < 8) x = 8;
        gfx_raised(x, y, 480, 350, COL_GRAY);
        gfx_fill(x + 3, y + 3, 474, 22, COL_NAVY);
        gfx_text(x + 10, y + 6, "GeometricalOS Startup Recovery", COL_WHITE, COL_NAVY);
        gfx_text(x + 20, y + 38, "Choose an option with the arrow keys and Enter.", COL_BLACK, COL_GRAY);
        for (int i = 0; i < count; i++) {
            int iy = y + 72 + i * 36;
            if (i == selected) gfx_fill(x + 16, iy, 448, 28, COL_NAVY);
            gfx_text(x + 26, iy + 6, items[i],
                     i == selected ? COL_WHITE : COL_BLACK,
                     i == selected ? COL_NAVY : COL_GRAY);
        }
        gfx_text(x + 20, y + 310,
                 fs_result == 0 ? "F8 was pressed during startup." :
                                  "Disk/filesystem startup failed; settings are unavailable.",
                 COL_DKGRAY, COL_GRAY);
        gfx_present();

        input_poll();
        KeyEvent k;
        while (input_key(&k)) {
            if (k.code == KEY_UP && selected > 0) selected--;
            else if (k.code == KEY_DOWN && selected + 1 < count) selected++;
            else if (k.code == 27) return BOOT_NORMAL;
            else if (k.code == 10 || k.code == 13) {
                if (fs_result != 0) {
                    if (selected == 0) {
                        recovery_details(bi, disk, disk_result, fs_result);
                        break;
                    }
                    reboot();
                }
                if (selected == 0) return BOOT_NORMAL;
                if (selected == 1) return BOOT_SAFE;
                if (selected == 2) {
                    if (settings_restore_backup() == 0) return BOOT_NORMAL;
                    gui_dialog("Recovery", "No usable settings backup was found.", "OK", 0, 0);
                } else if (selected == 3) {
                    settings_reset_defaults();
                    if (settings_save() == 0) return BOOT_NORMAL;
                    gui_dialog("Recovery", "Could not save default settings.", "OK", 0, 0);
                } else if (selected == 4) {
                    recovery_details(bi, disk, disk_result, fs_result);
                    break;
                } else reboot();
            }
        }
        MouseEvent m;
        while (input_mouse(&m)) {
            if (m.type != MOUSE_LDOWN) continue;
            int x = (gfx_width() - 480) / 2;
            int y = (gfx_height() - 350) / 2;
            int count = fs_result == 0 ? 6 : 2;
            int row = (m.y - (y + 72)) / 36;
            if (m.x < x + 16 || m.x >= x + 464 || row < 0 || row >= count) continue;
            selected = row;
            break;
        }
    }
}

static void recovery_event(Window *w, Event *e) {
    (void)w;
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    const BootInfo *bi = bootinfo();
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 10, "System Information and Recovery", COL_BLACK, COL_GRAY);
        char line[88];
        strcpy(line, bi->from_hdd ? "Boot source: hard disk" : "Boot source: CD/DVD");
        gfx_text(cx + 16, cy + 38, line, COL_BLACK, COL_GRAY);
        int p = (int)strlen("Video: ");
        strcpy(line, "Video: ");
        p = append_dec(line, p, bi->width);
        strcpy(line + p, " x "); p += 3;
        p = append_dec(line, p, bi->height);
        strcpy(line + p, " x "); p += 3;
        p = append_dec(line, p, bi->bpp);
        strcpy(line + p, " bpp");
        gfx_text(cx + 16, cy + 58, line, COL_BLACK, COL_GRAY);
        strcpy(line, "BIOS memory: ");
        p = append_dec(line, (int)strlen(line), bi->ext_mem_kb);
        strcpy(line + p, " KB above 1 MiB");
        gfx_text(cx + 16, cy + 78, line, COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 98, "Primary IDE:", COL_BLACK, COL_GRAY);
        gfx_text(cx + 112, cy + 98,
                 info_disk_result == 0 && info_disk.present ? info_disk.model : "not detected",
                 info_disk_result == 0 ? COL_NAVY : COL_MAROON, COL_GRAY);
        gfx_text(cx + 16, cy + 128, status, COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 158, "F8 at startup opens the recovery menu.", COL_DKGRAY, COL_GRAY);
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) {
            if (settings_restore_backup() == 0) {
                wall_load();
                strncpy(status, "Restored last good settings.", sizeof(status));
            } else {
                strncpy(status, "No usable settings backup.", sizeof(status));
            }
        } else if (e->menu_id == 2) {
            settings_reset_defaults();
            if (settings_save() == 0) {
                wall_load();
                strncpy(status, "Default settings saved.", sizeof(status));
            } else strncpy(status, "Could not save defaults.", sizeof(status));
        } else if (e->menu_id == 3) gui_close(win);
    }
}

void app_recovery_open(void) {
    if (win) { gui_restore(win); return; }
    memset(&info_disk, 0, sizeof(info_disk));
    info_disk_result = ata_probe(&info_disk);
    status[0] = 0;
    win = gui_create("System Information", 440, 250, ICON_SYSTEM, recovery_event, &win);
    if (!win) return;
    window_add_menu(win, "Recovery");
    window_add_item(win, 0, "Restore Last Good Settings", 1);
    window_add_item(win, 0, "Reset Settings to Defaults", 2);
    window_add_item(win, 0, "Close", 3);
    serial_write("APP: RECOVERY\n");
}
