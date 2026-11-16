#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "rtc.h"
#include "io.h"
#include "sound.h"
#include "wallpaper.h"
#include "settings.h"
#include "serial.h"
#include "fs.h"
#include "version.h"
#include <stdint.h>

static Window *pm;
static Window *groups[3];
static Window *desktop_menu;
static int sel_pm;
static int sel_group[3];
static int desktop_selected;
static int desktop_drag = -1;
static int desktop_dx, desktop_dy;
static int desktop_x, desktop_y;

static int clamp_desktop_x(int x) {
    int tw = gfx_text_width("Recycle Bin");
    int min_x = (tw + 1) / 2 - 8;
    int max_x = gfx_width() - 24 - tw / 2;
    if (min_x < 0) min_x = 0;
    if (max_x > gfx_width() - 32) max_x = gfx_width() - 32;
    if (max_x < min_x) max_x = min_x;
    if (x < min_x) return min_x;
    if (x > max_x) return max_x;
    return x;
}

static int clamp_desktop_y(int y) {
    int max_y = gfx_height() - GUI_TASKBAR_HEIGHT - 54;
    if (max_y < 0) max_y = 0;
    if (y < 0) return 0;
    if (y > max_y) return max_y;
    return y;
}

typedef struct {
    const char *name;
    int icon;
    void (*open)(void);
} Prog;

static const Prog main_items[] = {
    {"File Manager", ICON_FILES, app_files_open},
    {"Recycle Bin", ICON_RECYCLE, app_recycle_open},
};
static const Prog acc_items[] = {
    {"Notepad", ICON_NOTE, app_notepad_open},
    {"Write", ICON_WRITE, app_write_open},
    {"Calculator", ICON_CALC, app_calc_open},
    {"Clock", ICON_CLOCK, app_clock_open},
    {"Paint", ICON_PAINT, app_paint_open},
    {"Terminal", ICON_TERM, app_term_open},
    {"Control Panel", ICON_CONTROL, app_control_open},
    {"Media Player", ICON_MEDIA, app_player_open},
    {"Character Map", ICON_CHARMAP, app_charmap_open},
    {"Cardfile", ICON_CARDFILE, app_cardfile_open},
    {"System Information", ICON_SYSTEM, app_recovery_open},
    {"GeoFind", ICON_GEOFIND, app_geofind_open},
    {"Recent Documents", ICON_RECENT, app_recent_open},
    {"Run...", ICON_RUN, app_run_open},
    {"Help", ICON_HELP, app_help_open},
    {"Disk Space & Backup", ICON_STORAGE, app_storage_open},
    {"FTP Server", ICON_FTP, app_ftp_open},
};
static const Prog game_items[] = {
    {"Minesweeper", ICON_MINE, app_mines_open},
    {"Solitaire", ICON_SOL, app_sol_open},
    {"Snake", ICON_SNAKE, app_snake_open},
};

static const Prog *group_items(int g, int *n) {
    if (g == 0) { *n = 2; return main_items; }
    if (g == 1) { *n = 17; return acc_items; }
    *n = 3;
    return game_items;
}

static void launch_key(int key) {
    if (key == 2) app_files_open();
    else if (key == 3) app_notepad_open();
    else if (key == 4) app_calc_open();
    else if (key == 5) app_clock_open();
    else if (key == 6) app_paint_open();
    else if (key == 7) app_mines_open();
    else if (key == 8) app_term_open();
    else if (key == 9) app_sol_open();
    else if (key == 10) app_control_open();
    else if (key == 11) app_player_open();
    else if (key == 12) app_snake_open();
}

static void desktop_menu_event(Window *w, Event *e) {
    static const char *items[] = {
        "Solid",
        "Tiles",
        "Stripes",
        "Diamonds",
        "Orbit Grid"
    };
    const int item_count = (int)(sizeof(items) / sizeof(items[0]));
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        for (int i = 0; i < item_count; i++)
            gfx_button(cx + 6, cy + 4 + i * 26, cw - 12, 24, items[i], 0);
        return;
    }
    if (e->type != EVT_DOWN || e->x < 6 || e->x >= cw - 6) return;
    int item = (e->y - 4) / 26;
    if (item < 0 || item >= item_count || e->y >= 4 + item * 26 + 24) return;
    gui_close(w);
    wall_set(item);
}

static void open_desktop_menu(int x, int y) {
    if (desktop_menu) gui_close(desktop_menu);
    int w = 160, h = 4 + 5 * 26 + 4 + 24;
    if (w > gfx_width()) w = gfx_width();
    if (h > gfx_height()) h = gfx_height();
    desktop_menu = gui_create("Desktop", w, h, ICON_GROUP,
                              desktop_menu_event, &desktop_menu);
    if (!desktop_menu) return;
    if (x + w > gfx_width()) x = gfx_width() - w;
    if (y + h > gfx_height()) y = gfx_height() - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    gui_move(desktop_menu, x, y);
}

static void desktop_draw(void) {
    int x = clamp_desktop_x(desktop_x);
    int y = clamp_desktop_y(desktop_y);
    if (desktop_selected && gui_desktop_focused())
        gfx_rect(x - 3, y - 3, 38, 40, COL_WHITE);
    icons_draw(ICON_RECYCLE, x, y);
    int tw = gfx_text_width("Recycle Bin");
    int tx = x + 16 - tw / 2;
    if (desktop_selected)
        gfx_fill(tx - 2, y + 36, tw + 4, gfx_font_h(), COL_NAVY);
    gfx_text(tx, y + 36, "Recycle Bin", COL_WHITE,
             desktop_selected ? COL_NAVY : settings_desktop_color());
}

static void desktop_event(Event *e) {
    if (e->type == EVT_RDOWN) {
        open_desktop_menu(e->x, e->y);
    } else if (e->type == EVT_DOWN) {
        int x = clamp_desktop_x(desktop_x);
        int y = clamp_desktop_y(desktop_y);
        int tx = x + 16 - gfx_text_width("Recycle Bin") / 2;
        int left = tx < x - 8 ? tx : x - 8;
        int right = tx + gfx_text_width("Recycle Bin");
        if (right < x + 40) right = x + 40;
        if (e->x >= left && e->x < right &&
            e->y >= y - 2 && e->y < y + 36 + gfx_font_h()) {
            desktop_selected = 1;
            desktop_drag = 1;
            desktop_dx = e->x - x;
            desktop_dy = e->y - y;
            if (e->dbl) app_recycle_open();
            gui_invalidate();
            return;
        }
        desktop_drag = -1;
        gui_invalidate();
    } else if (e->type == EVT_MOVE && desktop_drag >= 0) {
        int x = e->x - desktop_dx, y = e->y - desktop_dy;
        desktop_x = clamp_desktop_x(x);
        desktop_y = clamp_desktop_y(y);
        gui_invalidate();
    } else if (e->type == EVT_UP) {
        if (desktop_drag >= 0)
            settings_set_icon_position(4, desktop_x, desktop_y);
        desktop_drag = -1;
    } else if (e->type == EVT_KEY) {
        if (e->code == 9 || e->code == KEY_RIGHT || e->code == KEY_DOWN ||
            e->code == KEY_LEFT || e->code == KEY_UP)
            desktop_selected = 1;
        else if (e->code == 10 || e->code == 13) app_recycle_open();
        else if (e->code == 27 && pm) gui_focus(pm);
        gui_invalidate();
    }
}

static void ask_exit(void) {
    int r = gui_dialog("Exit GeometricalOS",
                       "This will end your GeometricalOS session.",
                       "Shutdown", "Restart", "Cancel");
    if (r == 1) session_shutdown();
    else if (r == 2) session_restart();
}

static int split_icon_label(const char *label, char *first, char *second) {
    int len = (int)strlen(label);
    if (len <= 12) {
        memcpy(first, label, (size_t)len + 1);
        second[0] = 0;
        return 1;
    }

    int split = -1;
    int best = 1000;
    for (int i = 1; i < len - 1; i++) {
        if (label[i] != ' ') continue;
        int left = i;
        int right = len - i - 1;
        int difference = left > right ? left - right : right - left;
        if (left <= 12 && right <= 12 && difference < best) {
            split = i;
            best = difference;
        }
    }
    if (split < 0) {
        memcpy(first, label, (size_t)len + 1);
        second[0] = 0;
        return 1;
    }
    memcpy(first, label, (size_t)split);
    first[split] = 0;
    strcpy(second, label + split + 1);
    return 2;
}

static void draw_labeled_icon(int x, int y, int icon, const char *label, int selected,
                              int left, int right) {
    char first[24], second[24];
    int lines = split_icon_label(label, first, second);
    icons_draw(icon, x, y);
    for (int line = 0; line < lines; line++) {
        const char *text = line ? second : first;
        int tw = gfx_text_width(text);
        int tx = x + 16 - tw / 2;
        if (tx < left) tx = left;
        if (tx + tw > right) tx = right - tw;
        int ty = y + 36 + line * gfx_font_h();
        if (selected) gfx_fill(tx - 2, ty, tw + 4, gfx_font_h(), COL_NAVY);
        gfx_text(tx, ty, text, selected ? COL_WHITE : COL_BLACK,
                 selected ? COL_NAVY : COL_GRAY);
    }
}

static void open_group(int g);

static void pm_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    static const char *names[] = {"Workspace", "Utilities", "Arcade"};
    int xs[3];
    int gap = cw / 4;
    for (int i = 0; i < 3; i++) xs[i] = cx + gap * (i + 1) - 16;
    int iy = cy + 36;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, "Choose a workspace", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 3; i++)
            draw_labeled_icon(xs[i], iy, ICON_GROUP, names[i], sel_pm == i,
                              cx + 4, cx + cw - 4);
        gfx_sunken(cx + 4, cy + ch - 22, cw - 8, 18, COL_GRAY);
        gfx_text(cx + 8, cy + ch - 20, "F2 Files  F3 Note  F4 Calc  F5 Clock  F6 Paint  F7 Mine  F8 Term  F9 Sol",
                 COL_BLACK, COL_GRAY);
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1 || e->menu_id == -1) ask_exit();
        else if (e->menu_id == 3) app_wall_open();
        else if (e->menu_id == 4) app_run_open();
        else if (e->menu_id == 5) app_geofind_open();
        else if (e->menu_id == 6) app_recent_open();
        else if (e->menu_id == 7) app_storage_open();
        else if (e->menu_id == 2) {
            gui_dialog("About GeometricalOS",
                   "GeometricalOS version " GOS_VERSION "\n\n"
                       "A tiny hobby operating system\n"
                       "inspired by Windows 3.1.\n\n"
                       "Write, Character Map, Cardfile,\n"
                       "GeoFind, Run, Help, Backups, FTP,\n"
                       "Snake, Solitaire, Calculator,\n"
                       "Clock, Paint, and files.",
                       "OK", 0, 0);
        } else if (e->menu_id == 8) app_help_open();
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < 3; i++) {
            if (e->x >= xs[i] - cx - 8 && e->x < xs[i] - cx + 48 && e->y >= 36 && e->y < 36 + 56) {
                sel_pm = i;
                if (e->dbl) open_group(i);
                return;
            }
        }
    }
    if (e->type == EVT_KEY) {
        if (e->code == KEY_LEFT && sel_pm > 0) sel_pm--;
        else if (e->code == KEY_RIGHT && sel_pm < 2) sel_pm++;
        else if (e->code == 9) sel_pm = (sel_pm + 1) % 3;
        else if (e->code == 10 || e->code == 13) open_group(sel_pm);
    }
}

static void group_event(Window *w, Event *e) {
    int g = (int)(intptr_t)w->user;
    int n = 0;
    const Prog *items = group_items(g, &n);
    int *sel = &sel_group[g];
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        for (int i = 0; i < n; i++) {
            int col = i % 4;
            int row = i / 4;
            int x = cx + 24 + col * 110;
            int y = cy + 16 + row * 80;
            draw_labeled_icon(x, y, items[i].icon, items[i].name, *sel == i,
                              cx + 4, cx + cw - 4);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < n; i++) {
            int col = i % 4;
            int row = i / 4;
            int x = 24 + col * 110;
            int y = 16 + row * 80;
            int label_lines = (int)strlen(items[i].name) > 12 ? 2 : 1;
            if (e->x >= x - 8 && e->x < x + 80 &&
                e->y >= y && e->y < y + 36 + label_lines * gfx_font_h()) {
                *sel = i;
                if (e->dbl) items[i].open();
                return;
            }
        }
    }
    if (e->type == EVT_KEY) {
        if (e->code == KEY_LEFT && *sel > 0) (*sel)--;
        else if (e->code == KEY_RIGHT && *sel + 1 < n) (*sel)++;
        else if (e->code == KEY_UP && *sel >= 4) *sel -= 4;
        else if (e->code == KEY_DOWN && *sel + 4 < n) *sel += 4;
        else if (e->code == 9 && n) *sel = (*sel + 1) % n;
        else if (e->code == 10 || e->code == 13) items[*sel].open();
    }
}

static void open_group(int g) {
    if (groups[g]) { gui_restore(groups[g]); return; }
    const char *title = g == 0 ? "Workspace" : (g == 1 ? "Utilities" : "Arcade");
    int n = 0;
    group_items(g, &n);
    int ww = 140 + (n > 4 ? 4 : n) * 100;
    if (ww < 280) ww = 280;
    if (ww > gfx_width() - 30) ww = gfx_width() - 30;
    int rows = (n + 3) / 4;
    if (rows < 1) rows = 1;
    int wh = 80 + rows * 96;
    if (wh > gfx_height() - GUI_TASKBAR_HEIGHT - 16)
        wh = gfx_height() - GUI_TASKBAR_HEIGHT - 16;
    groups[g] = gui_create(title, ww, wh, ICON_GROUP, group_event, &groups[g]);
    if (!groups[g]) return;
    groups[g]->user = (void *)(intptr_t)g;
    serial_write(g == 0 ? "GROUP: MAIN\n" : (g == 1 ? "GROUP: ACC\n" : "GROUP: GAMES\n"));
}

static void open_geodeck(void) {
    if (pm) { gui_restore(pm); return; }
    int ww = gfx_width() >= 800 ? 640 : gfx_width() - 48;
    int wh = gfx_height() >= 600 ? 420 : gfx_height() - GUI_TASKBAR_HEIGHT - 48;
    if (ww < 280) ww = 280;
    if (wh < 180) wh = 180;
    pm = gui_create("GeoDeck", ww, wh, ICON_GROUP, pm_event, &pm);
    if (!pm) return;
    gui_move(pm, (gfx_width() - pm->w) / 2,
             (gfx_height() - GUI_TASKBAR_HEIGHT - pm->h) / 2);
    window_add_menu(pm, "File");
    window_add_item(pm, 0, "Exit GeometricalOS...", 1);
    window_add_item(pm, 0, "Run...", 4);
    window_add_item(pm, 0, "GeoFind...", 5);
    window_add_item(pm, 0, "Recent Documents", 6);
    window_add_item(pm, 0, "Disk Space & Backup", 7);
    window_add_menu(pm, "Options");
    window_add_item(pm, 1, "Wallpaper...", 3);
    window_add_menu(pm, "Help");
    window_add_item(pm, 2, "Help Topics", 8);
    window_add_item(pm, 2, "About GeometricalOS", 2);
}

void splash_screen(void) {
    sound_startup();
    gfx_reset_clip();
    gfx_clear(COL_NAVY);
    int cx = gfx_width() / 2;
    int cy = gfx_height() / 2 - 30;
    gfx_logo(cx, cy - 50, 2);
    const char *name = "GeometricalOS";
    int scale = gfx_width() >= 800 ? 3 : 2;
    int tw = (int)strlen(name) * 8 * scale;
    gfx_text_scaled(cx - tw / 2, cy + 40, name, COL_WHITE, scale);
    const char *ver = "Version " GOS_VERSION;
    gfx_text(cx - gfx_text_width(ver) / 2, cy + 48 + gfx_font_h() * scale, ver, COL_YELLOW, COL_NAVY);
    if (settings_safe_mode_active()) {
        const char *mode = "SAFE MODE";
        gfx_text(cx - gfx_text_width(mode) / 2, cy + 48 + gfx_font_h() * scale * 2,
                 mode, COL_YELLOW, COL_NAVY);
    }
    gfx_present();
    sleep_seconds(2);
}

void desktop_init(void) {
    wall_load();
    gui_set_launcher(launch_key);
    gui_set_start_handler(open_geodeck);
    gui_set_desktop_handler(desktop_draw, desktop_event);
    desktop_x = clamp_desktop_x(settings_icon_x(4));
    desktop_y = clamp_desktop_y(settings_icon_y(4));
}
