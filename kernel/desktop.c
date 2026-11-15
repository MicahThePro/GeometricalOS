#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "rtc.h"
#include "io.h"
#include "sound.h"
#include "wallpaper.h"
#include "serial.h"

static Window *pm;
static Window *groups[3];
static int sel_pm;
static int sel_group[3];

typedef struct {
    const char *name;
    int icon;
    void (*open)(void);
} Prog;

static const Prog main_items[] = {
    {"File Manager", ICON_FILES, app_files_open},
};
static const Prog acc_items[] = {
    {"Notepad", ICON_NOTE, app_notepad_open},
    {"Calculator", ICON_CALC, app_calc_open},
    {"Clock", ICON_CLOCK, app_clock_open},
    {"Paint", ICON_PAINT, app_paint_open},
    {"Terminal", ICON_TERM, app_term_open},
};
static const Prog game_items[] = {
    {"Minesweeper", ICON_MINE, app_mines_open},
    {"Solitaire", ICON_SOL, app_sol_open},
};

static const Prog *group_items(int g, int *n) {
    if (g == 0) { *n = 1; return main_items; }
    if (g == 1) { *n = 5; return acc_items; }
    *n = 2;
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
}

static void ask_exit(void) {
    int r = gui_dialog("Exit GeometricalOS",
                       "This will end your GeometricalOS session.",
                       "Shutdown", "Restart", "Cancel");
    if (r == 1) session_shutdown();
    else if (r == 2) session_restart();
}

static void draw_labeled_icon(int x, int y, int icon, const char *label, int selected) {
    icons_draw(icon, x, y);
    int tw = gfx_text_width(label);
    int tx = x + 16 - tw / 2;
    if (tx < x - 20) tx = x - 20;
    if (selected) gfx_fill(tx - 2, y + 36, tw + 4, gfx_font_h(), COL_NAVY);
    gfx_text(tx, y + 36, label, selected ? COL_WHITE : COL_BLACK,
             selected ? COL_NAVY : COL_GRAY);
}

static void open_group(int g);

static void pm_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    static const char *names[] = {"Main", "Accessories", "Games"};
    int xs[3];
    int gap = cw / 4;
    for (int i = 0; i < 3; i++) xs[i] = cx + gap * (i + 1) - 16;
    int iy = cy + 36;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, "Program groups", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 3; i++)
            draw_labeled_icon(xs[i], iy, ICON_GROUP, names[i], sel_pm == i);
        gfx_sunken(cx + 4, cy + ch - 22, cw - 8, 18, COL_GRAY);
        gfx_text(cx + 8, cy + ch - 20, "F2 Files  F3 Note  F4 Calc  F5 Clock  F6 Paint  F7 Mine  F8 Term  F9 Sol",
                 COL_BLACK, COL_GRAY);
        return;
    }
    if (e->type == EVT_MENU || (e->type == EVT_MENU && e->menu_id == -1)) {
        if (e->menu_id == 1 || e->menu_id == -1) ask_exit();
        else if (e->menu_id == 3) app_wall_open();
        else if (e->menu_id == 2) {
            gui_dialog("About GeometricalOS",
                       "GeometricalOS version 1.0\n\n"
                       "A tiny hobby operating system\n"
                       "inspired by Windows 3.1.\n\n"
                       "Notepad, Paint, Minesweeper,\n"
                       "Calculator, Clock, and files.",
                       "OK", 0, 0);
        }
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
            draw_labeled_icon(x, y, items[i].icon, items[i].name, *sel == i);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < n; i++) {
            int col = i % 4;
            int row = i / 4;
            int x = 24 + col * 110;
            int y = 16 + row * 80;
            if (e->x >= x - 8 && e->x < x + 80 && e->y >= y && e->y < y + 56) {
                *sel = i;
                if (e->dbl) items[i].open();
                return;
            }
        }
    }
    if (e->type == EVT_KEY) {
        if (e->code == KEY_LEFT && *sel > 0) (*sel)--;
        else if (e->code == KEY_RIGHT && *sel + 1 < n) (*sel)++;
        else if (e->code == 10 || e->code == 13) items[*sel].open();
    }
}

static void open_group(int g) {
    if (groups[g]) { gui_restore(groups[g]); return; }
    const char *title = g == 0 ? "Main" : (g == 1 ? "Accessories" : "Games");
    int n = 0;
    group_items(g, &n);
    int ww = 140 + (n > 4 ? 4 : n) * 100;
    if (ww < 280) ww = 280;
    if (ww > gfx_width() - 30) ww = gfx_width() - 30;
    int rows = (n + 3) / 4;
    if (rows < 1) rows = 1;
    int wh = 80 + rows * 96;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    groups[g] = gui_create(title, ww, wh, ICON_GROUP, group_event, &groups[g]);
    if (!groups[g]) return;
    groups[g]->user = (void *)(intptr_t)g;
    serial_write(g == 0 ? "GROUP: MAIN\n" : (g == 1 ? "GROUP: ACC\n" : "GROUP: GAMES\n"));
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
    const char *ver = "Version 1.0";
    gfx_text(cx - gfx_text_width(ver) / 2, cy + 48 + gfx_font_h() * scale, ver, COL_YELLOW, COL_NAVY);
    gfx_present();
    sleep_seconds(2);
}

void desktop_init(void) {
    wall_load();
    gui_set_launcher(launch_key);
    int ww = gfx_width() >= 800 ? 640 : gfx_width() - 40;
    int wh = gfx_height() >= 600 ? 420 : gfx_height() - 70;
    pm = gui_create("Program Manager", ww, wh, ICON_GROUP, pm_event, &pm);
    if (!pm) return;
    pm->keep = 1;
    gui_move(pm, 24, 16);
    window_add_menu(pm, "File");
    window_add_item(pm, 0, "Exit GeometricalOS...", 1);
    window_add_menu(pm, "Options");
    window_add_item(pm, 1, "Wallpaper...", 3);
    window_add_menu(pm, "Help");
    window_add_item(pm, 2, "About GeometricalOS", 2);
    /* Open the main group so a program is one Enter away, and also
       leave Program Manager in front. */
    open_group(0);
    gui_focus(pm);
}
