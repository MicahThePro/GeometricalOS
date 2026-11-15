#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "rtc.h"
#include "wallpaper.h"
#include "io.h"
#include "fs.h"
#include "settings.h"
#include "sound.h"

#define BORDER 3
#define TITLE_H 18
#define MENU_H 18
#define MAXW 20

static Window wins[MAXW];
static Window *focused;
static int zseq;
static int dirty = 1;
static int cascade;

void gui_pump(void);

static int drag; /* 1 = move window, 2 = client capture */
static Window *drag_win;
static int drag_dx, drag_dy;

static int menu_on;
static Window *menu_win;
static int menu_i, menu_hover = -1;

static int sys_on;
static Window *sys_win;

static void (*launch_fn)(int fkey);

static uint32_t click_tick;
static int click_x, click_y;

enum { HIT_NONE, HIT_SYS, HIT_MIN, HIT_MAX, HIT_CLOSE, HIT_TITLE, HIT_MENU, HIT_CLIENT, HIT_BAR };

static int iabs(int v) { return v < 0 ? -v : v; }

void gui_set_launcher(void (*fn)(int fkey)) { launch_fn = fn; }

void gui_invalidate(void) { dirty = 1; }

static int modal_active(void);

void window_client(Window *w, int *x, int *y, int *cw, int *ch) {
    *x = w->x + BORDER;
    *y = w->y + BORDER + TITLE_H + (w->menu_count ? MENU_H : 0);
    *cw = w->w - BORDER * 2;
    *ch = w->h - BORDER * 2 - TITLE_H - (w->menu_count ? MENU_H : 0);
    if (*cw < 0) *cw = 0;
    if (*ch < 0) *ch = 0;
}

void window_add_menu(Window *w, const char *label) {
    if (w->menu_count >= MENU_MAX) return;
    MenuCol *m = &w->menus[w->menu_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->label, label, sizeof(m->label));
}

void window_add_item(Window *w, int menu, const char *label, int id) {
    if (menu < 0 || menu >= w->menu_count) return;
    MenuCol *m = &w->menus[menu];
    if (m->count >= ITEM_MAX) return;
    strncpy(m->items[m->count], label, 28);
    m->ids[m->count] = id;
    m->count++;
}

void gui_focus(Window *w) {
    if (!w || !w->used) return;
    w->z = ++zseq;
    focused = w;
    gui_invalidate();
}

void gui_move(Window *w, int x, int y) {
    if (!w) return;
    w->x = x;
    w->y = y;
    gui_invalidate();
}

void gui_restore(Window *w) {
    if (!w || !w->used) return;
    w->minimized = 0;
    gui_focus(w);
}

void gui_close(Window *w) {
    if (!w || !w->used || w->keep) return;
    if (menu_win == w) menu_on = 0;
    if (sys_win == w) sys_on = 0;
    if (drag_win == w) { drag = 0; drag_win = 0; }
    w->used = 0;
    if (w->owner) *w->owner = 0;
    if (focused == w) {
        focused = 0;
        int best = -1;
        for (int i = 0; i < MAXW; i++) {
            if (!wins[i].used || wins[i].minimized) continue;
            if (!focused || wins[i].z >= best) {
                focused = &wins[i];
                best = wins[i].z;
            }
        }
    }
    gui_invalidate();
}

Window *gui_create(const char *title, int w, int h, int icon,
                   void (*event)(Window *, Event *), Window **owner) {
    Window *win = 0;
    for (int i = 0; i < MAXW; i++) if (!wins[i].used) { win = &wins[i]; break; }
    if (!win) return 0;
    memset(win, 0, sizeof(*win));
    win->used = 1;
    if (w > gfx_width()) w = gfx_width();
    if (h > gfx_height()) h = gfx_height();
    if (w < 80) w = 80;
    if (h < 60) h = 60;
    win->w = w;
    win->h = h;
    win->x = 30 + (cascade % 7) * 22;
    win->y = 20 + (cascade % 7) * 22;
    if (win->x + 80 > gfx_width()) win->x = 16;
    if (win->y + 40 > gfx_height()) win->y = 16;
    cascade++;
    strncpy(win->title, title ? title : "", sizeof(win->title));
    win->event = event;
    win->icon = icon;
    win->owner = owner;
    if (owner) *owner = win;
    gui_focus(win);
    return win;
}

static void send(Window *w, Event *e) {
    if (w && w->used && w->event) w->event(w, e);
}

static void request_close(Window *w) {
    if (!w || !w->used) return;
    if (w->ask_close && !w->ask_close(w)) return;
    if (w->keep) {
        Event e;
        memset(&e, 0, sizeof(e));
        e.type = EVT_MENU;
        e.menu_id = -1;
        send(w, &e);
        return;
    }
    gui_close(w);
}

static void toggle_max(Window *w) {
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = gfx_width(); w->h = gfx_height();
        w->maximized = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = 0;
    }
    w->minimized = 0;
    gui_focus(w);
}

static int hit_window(Window *w, int mx, int my, int *menu_index) {
    if (w->minimized) return HIT_NONE;
    if (mx < w->x || my < w->y || mx >= w->x + w->w || my >= w->y + w->h) return HIT_NONE;
    int tx = w->x + BORDER;
    int ty = w->y + BORDER;
    int tw = w->w - BORDER * 2;
    if (my >= ty && my < ty + TITLE_H) {
        if (mx >= tx + 2 && mx < tx + 18 && my >= ty + 1 && my < ty + TITLE_H - 1) return HIT_SYS;
        int close_x = tx + tw - 18;
        int max_x = close_x - 18;
        int min_x = max_x - 18;
        if (mx >= close_x && mx < close_x + 16) return HIT_CLOSE;
        if (mx >= max_x && mx < max_x + 16) return HIT_MAX;
        if (mx >= min_x && mx < min_x + 16) return HIT_MIN;
        return HIT_TITLE;
    }
    int bar = ty + TITLE_H;
    if (w->menu_count && my >= bar && my < bar + MENU_H) {
        int x = tx + 4;
        for (int i = 0; i < w->menu_count; i++) {
            int lw = gfx_text_width(w->menus[i].label) + 14;
            if (mx >= x && mx < x + lw) {
                if (menu_index) *menu_index = i;
                return HIT_MENU;
            }
            x += lw;
        }
        return HIT_BAR;
    }
    return HIT_CLIENT;
}

static void dropdown_rect(Window *w, int index, int *x, int *y, int *wd, int *ht) {
    int lx = w->x + BORDER + 4;
    for (int i = 0; i < index; i++)
        lx += gfx_text_width(w->menus[i].label) + 14;
    int mw = 140;
    for (int i = 0; i < w->menus[index].count; i++) {
        int tw = gfx_text_width(w->menus[index].items[i]) + 28;
        if (tw > mw) mw = tw;
    }
    *x = lx;
    *y = w->y + BORDER + TITLE_H + MENU_H - 1;
    *wd = mw;
    *ht = w->menus[index].count * 20 + 6;
    if (*x + *wd > gfx_width()) *x = gfx_width() - *wd - 4;
    if (*x < 2) *x = 2;
}

static void glyph_box(int x, int y, int pressed) {
    gfx_button(x, y, 16, 14, 0, pressed);
}

static void paint_frame(Window *w) {
    uint32_t title_bg = (w == focused) ? settings_title_color() : COL_DKGRAY;
    gfx_raised(w->x, w->y, w->w, w->h, COL_GRAY);
    int tx = w->x + BORDER;
    int ty = w->y + BORDER;
    int tw = w->w - BORDER * 2;
    gfx_fill(tx, ty, tw, TITLE_H, title_bg);
    glyph_box(tx + 2, ty + 2, 0);
    gfx_hline(tx + 5, ty + 8, 8, COL_BLACK);
    int close_x = tx + tw - 18;
    int max_x = close_x - 18;
    int min_x = max_x - 18;
    glyph_box(min_x, ty + 2, 0);
    glyph_box(max_x, ty + 2, 0);
    glyph_box(close_x, ty + 2, 0);
    gfx_hline(min_x + 4, ty + 11, 8, COL_BLACK);
    if (w->maximized) {
        gfx_rect(max_x + 5, ty + 5, 6, 6, COL_BLACK);
        gfx_rect(max_x + 7, ty + 3, 6, 6, COL_BLACK);
    } else {
        gfx_rect(max_x + 4, ty + 4, 8, 8, COL_BLACK);
    }
    gfx_line(close_x + 4, ty + 5, close_x + 11, ty + 12, COL_BLACK);
    gfx_line(close_x + 11, ty + 5, close_x + 4, ty + 12, COL_BLACK);
    int text_x = tx + 22;
    int limit = min_x - 6;
    for (int i = 0; w->title[i] && text_x + 8 <= limit; i++) {
        gfx_char(text_x, ty + 1, (unsigned char)w->title[i], COL_WHITE, title_bg, 1);
        text_x += 8;
    }
    if (w->menu_count) {
        int my = ty + TITLE_H;
        gfx_fill(tx, my, tw, MENU_H, COL_GRAY);
        gfx_hline(tx, my + MENU_H - 1, tw, COL_WHITE);
        int x = tx + 8;
        for (int i = 0; i < w->menu_count; i++) {
            int hot = (menu_on && menu_win == w && menu_i == i);
            int lw = gfx_text_width(w->menus[i].label);
            if (hot) gfx_fill(x - 4, my + 1, lw + 8, MENU_H - 3, COL_NAVY);
            gfx_text(x, my + 1, w->menus[i].label, hot ? COL_WHITE : COL_BLACK,
                     hot ? COL_NAVY : COL_GRAY);
            x += lw + 14;
        }
    }
}

static void paint_menu_dropdown(void) {
    if (!menu_on || !menu_win || !menu_win->used) return;
    int x, y, wd, ht;
    dropdown_rect(menu_win, menu_i, &x, &y, &wd, &ht);
    gfx_raised(x, y, wd, ht, COL_GRAY);
    MenuCol *m = &menu_win->menus[menu_i];
    for (int i = 0; i < m->count; i++) {
        int iy = y + 3 + i * 20;
        int hot = (i == menu_hover);
        if (hot) gfx_fill(x + 2, iy, wd - 4, 20, COL_NAVY);
        gfx_text(x + 10, iy + 2, m->items[i], hot ? COL_WHITE : COL_BLACK,
                 hot ? COL_NAVY : COL_GRAY);
    }
}

static void paint_sys_menu(void) {
    if (!sys_on || !sys_win || !sys_win->used) return;
    static const char *items[] = {"Restore", "Minimize", "Maximize", "Close"};
    int x = sys_win->x + BORDER + 2;
    int y = sys_win->y + BORDER + TITLE_H;
    int wd = 120, ht = 4 * 20 + 6;
    gfx_raised(x, y, wd, ht, COL_GRAY);
    for (int i = 0; i < 4; i++) {
        int iy = y + 3 + i * 20;
        int hot = (mouse_x >= x && mouse_x < x + wd && mouse_y >= iy && mouse_y < iy + 20);
        if (hot) gfx_fill(x + 2, iy, wd - 4, 20, COL_NAVY);
        gfx_text(x + 10, iy + 2, items[i], hot ? COL_WHITE : COL_BLACK,
                 hot ? COL_NAVY : COL_GRAY);
    }
}

static void paint_window(Window *w) {
    paint_frame(w);
    if (!w->event) return;
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (cw < 2 || ch < 2) return;
    gfx_set_clip(cx, cy, cw, ch);
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = EVT_PAINT;
    w->event(w, &e);
    gfx_reset_clip();
}

static void minimized_slot(int index, int *x, int *y) {
    int col = index % 8;
    int row = index / 8;
    *x = 8 + col * 78;
    *y = gfx_height() - 60 - row * 56;
}

static void paint_minimized(void) {
    for (int i = 0; i < MAXW; i++) {
        Window *w = &wins[i];
        if (!w->used || !w->minimized) continue;
        int x, y;
        minimized_slot(i, &x, &y);
        icons_draw(w->icon ? w->icon : ICON_GROUP, x, y);
        int hot = (focused == w);
        int lw = gfx_text_width(w->title);
        if (lw > 72) lw = 72;
        int tx = x + 16 - lw / 2;
        if (tx < 2) tx = 2;
        if (hot) gfx_fill(tx - 2, y + 34, lw + 4, gfx_font_h(), COL_NAVY);
        int shown = 0;
        for (int c = 0; w->title[c] && shown < 9; c++, shown++)
            gfx_char(tx + shown * 8, y + 34, (unsigned char)w->title[c],
                     hot ? COL_WHITE : COL_BLACK, hot ? COL_NAVY : COL_TEAL, 1);
    }
}

/* ---------- modal dialogs ---------- */

static struct {
    int on, result, nbtn;
    char title[40];
    char msg[360];
    char btn[3][16];
} dialog;

static struct {
    int on, result, len, max;
    char title[40];
    char label[64];
    char *buf;
} prompt;

static struct {
    int on, result, count, sel, scroll;
    char title[40];
    char names[48][28];
} pick;

static struct {
    int on, result, save, count, sel, scroll, namelen;
    uint16_t dir;
    char title[40];
    char name[28];
    int ids[40];
    int isdir[40];
    char labels[40][28];
} filedlg;

static void button_row(int x, int y, int w, int n, const char labels[][16], int *bx, int *bw);

static int modal_active(void) {
    return dialog.on || prompt.on || pick.on || filedlg.on;
}

static void file_refresh(void) {
    filedlg.count = 0;
    filedlg.sel = 0;
    filedlg.scroll = 0;
    if (filedlg.dir != FS_ROOT && filedlg.count < 40) {
        filedlg.ids[filedlg.count] = -1;
        filedlg.isdir[filedlg.count] = 1;
        strncpy(filedlg.labels[filedlg.count], "..", 28);
        filedlg.count++;
    }
    int ids[64];
    int n = fs_list(filedlg.dir, ids, 64);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n && filedlg.count < 40; i++) {
            int dir = fs_is_dir(ids[i]);
            if (pass == 0 && !dir) continue;
            if (pass == 1 && dir) continue;
            filedlg.ids[filedlg.count] = ids[i];
            filedlg.isdir[filedlg.count] = dir;
            strncpy(filedlg.labels[filedlg.count], fs_name(ids[i]), 28);
            filedlg.count++;
        }
    }
}

static void file_geom(int *x, int *y, int *w, int *h) {
    *w = 420;
    *h = 320;
    if (*w > gfx_width() - 16) *w = gfx_width() - 16;
    if (*h > gfx_height() - 16) *h = gfx_height() - 16;
    *x = (gfx_width() - *w) / 2;
    *y = (gfx_height() - *h) / 2;
}

static void paint_filedlg(void) {
    int x, y, w, h;
    file_geom(&x, &y, &w, &h);
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, TITLE_H, COL_NAVY);
    gfx_text(x + 10, y + 4, filedlg.title, COL_WHITE, COL_NAVY);
    gfx_text(x + 12, y + 28, filedlg.dir == FS_ROOT ? "Folder: \\" : "Folder", COL_BLACK, COL_GRAY);
    int lx = x + 12, ly = y + 48, lw = w - 24, lh = h - 130;
    gfx_sunken(lx, ly, lw, lh, COL_WHITE);
    int rows = (lh - 8) / 18;
    if (rows < 1) rows = 1;
    if (filedlg.sel < filedlg.scroll) filedlg.scroll = filedlg.sel;
    if (filedlg.sel >= filedlg.scroll + rows) filedlg.scroll = filedlg.sel - rows + 1;
    for (int i = 0; i < rows; i++) {
        int idx = filedlg.scroll + i;
        if (idx >= filedlg.count) break;
        int iy = ly + 4 + i * 18;
        int hot = (idx == filedlg.sel);
        if (hot) gfx_fill(lx + 2, iy, lw - 4, 18, COL_NAVY);
        char row[32];
        strncpy(row, filedlg.labels[idx], 24);
        row[24] = 0;
        if (filedlg.isdir[idx] && strcmp(row, "..") != 0) {
            int L = (int)strlen(row);
            if (L < 22) { row[L] = ' '; row[L + 1] = 0; }
        }
        gfx_text(lx + 6, iy + 1, row, hot ? COL_WHITE : COL_BLACK, hot ? COL_NAVY : COL_WHITE);
        if (filedlg.isdir[idx])
            gfx_text(lx + lw - 52, iy + 1, "<DIR>", hot ? COL_YELLOW : COL_NAVY, hot ? COL_NAVY : COL_WHITE);
    }
    gfx_text(x + 12, y + h - 72, "File name:", COL_BLACK, COL_GRAY);
    gfx_sunken(x + 100, y + h - 76, w - 116, 22, COL_WHITE);
    gfx_text(x + 104, y + h - 72, filedlg.name, COL_BLACK, COL_WHITE);
    int bx[2], bw;
    char labels[2][16];
    strncpy(labels[0], filedlg.save ? "OK" : "Open", 16);
    strncpy(labels[1], "Cancel", 16);
    button_row(x, y + h - 36, w, 2, labels, bx, &bw);
}

static int file_click(int mx, int my, int *row_out, int dbl) {
    int x, y, w, h;
    file_geom(&x, &y, &w, &h);
    int bw0 = 88, gap = 10, total = 2 * bw0 + gap;
    int sx = x + (w - total) / 2;
    int by = y + h - 36;
    for (int i = 0; i < 2; i++) {
        int bx = sx + i * (bw0 + gap);
        if (mx >= bx && mx < bx + bw0 && my >= by && my < by + 24) return i == 0 ? 1 : 0;
    }
    int lx = x + 12, ly = y + 48, lw = w - 24, lh = h - 130;
    if (mx >= lx && mx < lx + lw && my >= ly && my < ly + lh) {
        int rows = (lh - 8) / 18;
        int rel = (my - (ly + 4)) / 18;
        int idx = filedlg.scroll + rel;
        if (rel >= 0 && rel < rows && idx >= 0 && idx < filedlg.count) {
            if (row_out) *row_out = idx;
            if (dbl && filedlg.isdir[idx]) return 3;
            if (dbl) return 1;
            return 2;
        }
    }
    return -1;
}

int gui_file(const char *title, int save, uint16_t *dir, char *name, int namemax) {
    if (!dir || !name || namemax < 2) return 0;
    memset(&filedlg, 0, sizeof(filedlg));
    strncpy(filedlg.title, title ? title : (save ? "Save As" : "Open"), sizeof(filedlg.title));
    filedlg.save = save;
    filedlg.dir = *dir;
    strncpy(filedlg.name, name, 27);
    filedlg.namelen = (int)strlen(filedlg.name);
    file_refresh();
    filedlg.on = 1;
    menu_on = 0;
    sys_on = 0;
    for (;;) {
        filedlg.result = -1;
        gui_invalidate();
        while (filedlg.result < 0) gui_pump();
        if (filedlg.result == 0) {
            filedlg.on = 0;
            gui_invalidate();
            return 0;
        }
        if (!filedlg.name[0]) continue;
        int existing = fs_lookup(filedlg.dir, filedlg.name);
        if (!save) {
            if (existing < 0 || fs_is_dir(existing)) {
                gui_dialog("Open", "Choose a file in this folder.", "OK", 0, 0);
                continue;
            }
        } else if (existing >= 0 && !fs_is_dir(existing)) {
            int r = gui_dialog("Save As", "Replace the existing file?", "Yes", "No", 0);
            if (r != 1) continue;
        } else if (existing >= 0 && fs_is_dir(existing)) {
            filedlg.dir = (uint16_t)existing;
            filedlg.name[0] = 0;
            filedlg.namelen = 0;
            file_refresh();
            continue;
        }
        break;
    }
    *dir = filedlg.dir;
    strncpy(name, filedlg.name, (size_t)namemax);
    name[namemax - 1] = 0;
    filedlg.on = 0;
    gui_invalidate();
    return 1;
}

static void dialog_geom(int *x, int *y, int *w, int *h) {
    *w = 460;
    *h = 210;
    if (*w > gfx_width() - 16) *w = gfx_width() - 16;
    if (*h > gfx_height() - 16) *h = gfx_height() - 16;
    *x = (gfx_width() - *w) / 2;
    *y = (gfx_height() - *h) / 2;
}

static void prompt_geom(int *x, int *y, int *w, int *h) {
    *w = 420;
    *h = 160;
    if (*w > gfx_width() - 16) *w = gfx_width() - 16;
    *x = (gfx_width() - *w) / 2;
    *y = (gfx_height() - *h) / 2;
}

static void button_row(int x, int y, int w, int n, const char labels[][16], int *bx, int *bw) {
    int bw0 = 88, gap = 10;
    int total = n * bw0 + (n - 1) * gap;
    int sx = x + (w - total) / 2;
    for (int i = 0; i < n; i++) {
        bx[i] = sx + i * (bw0 + gap);
        gfx_button(bx[i], y, bw0, 24, labels[i], 0);
        if (i == 0) gfx_button_focus(bx[i], y, bw0, 24);
    }
    *bw = bw0;
}

static void paint_dialog(void) {
    int x, y, w, h;
    dialog_geom(&x, &y, &w, &h);
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, TITLE_H, COL_NAVY);
    gfx_text(x + 10, y + 4, dialog.title, COL_WHITE, COL_NAVY);
    int ty = y + 32;
    int tx = x + 16;
    for (int i = 0; dialog.msg[i] && ty < y + h - 40; i++) {
        if (dialog.msg[i] == '\n') { ty += gfx_font_h(); tx = x + 16; continue; }
        gfx_char(tx, ty, (unsigned char)dialog.msg[i], COL_BLACK, COL_GRAY, 1);
        tx += 8;
        if (tx > x + w - 16) { ty += gfx_font_h(); tx = x + 16; }
    }
    int bx[3], bw;
    char labels[3][16];
    for (int i = 0; i < dialog.nbtn; i++) strncpy(labels[i], dialog.btn[i], 16);
    button_row(x, y + h - 36, w, dialog.nbtn, labels, bx, &bw);
}

static void paint_prompt_box(void) {
    int x, y, w, h;
    prompt_geom(&x, &y, &w, &h);
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, TITLE_H, COL_NAVY);
    gfx_text(x + 10, y + 4, prompt.title, COL_WHITE, COL_NAVY);
    gfx_text(x + 16, y + 32, prompt.label, COL_BLACK, COL_GRAY);
    gfx_sunken(x + 16, y + 54, w - 32, 24, COL_WHITE);
    gfx_text(x + 20, y + 58, prompt.buf, COL_BLACK, COL_WHITE);
    int cx = x + 20 + prompt.len * 8;
    if ((timer_ticks() / 8) & 1) gfx_fill(cx, y + 58, 2, gfx_font_h() - 2, COL_BLACK);
    int bx[2], bw;
    char labels[2][16];
    strncpy(labels[0], "OK", 16);
    strncpy(labels[1], "Cancel", 16);
    button_row(x, y + h - 36, w, 2, labels, bx, &bw);
}

static int dialog_click(int mx, int my) {
    int x, y, w, h;
    dialog_geom(&x, &y, &w, &h);
    int bw0 = 88, gap = 10;
    int total = dialog.nbtn * bw0 + (dialog.nbtn - 1) * gap;
    int sx = x + (w - total) / 2;
    int by = y + h - 36;
    for (int i = 0; i < dialog.nbtn; i++) {
        int bx = sx + i * (bw0 + gap);
        if (mx >= bx && mx < bx + bw0 && my >= by && my < by + 24) return i + 1;
    }
    return 0;
}

static int prompt_click(int mx, int my) {
    int x, y, w, h;
    prompt_geom(&x, &y, &w, &h);
    int bw0 = 88, gap = 10;
    int total = 2 * bw0 + gap;
    int sx = x + (w - total) / 2;
    int by = y + h - 36;
    for (int i = 0; i < 2; i++) {
        int bx = sx + i * (bw0 + gap);
        if (mx >= bx && mx < bx + bw0 && my >= by && my < by + 24) return i == 0 ? 1 : 0;
    }
    return -1;
}

int gui_dialog(const char *title, const char *msg, const char *b1, const char *b2, const char *b3) {
    memset(&dialog, 0, sizeof(dialog));
    strncpy(dialog.title, title ? title : "", sizeof(dialog.title));
    strncpy(dialog.msg, msg ? msg : "", sizeof(dialog.msg));
    dialog.nbtn = 0;
    if (b1) strncpy(dialog.btn[dialog.nbtn++], b1, 16);
    if (b2) strncpy(dialog.btn[dialog.nbtn++], b2, 16);
    if (b3) strncpy(dialog.btn[dialog.nbtn++], b3, 16);
    if (!dialog.nbtn) strncpy(dialog.btn[dialog.nbtn++], "OK", 16);
    dialog.on = 1;
    dialog.result = -1;
    menu_on = 0;
    sys_on = 0;
    gui_invalidate();
    while (dialog.result < 0) gui_pump();
    dialog.on = 0;
    gui_invalidate();
    return dialog.result;
}

int gui_prompt(const char *title, const char *label, char *buf, int buflen) {
    if (!buf || buflen < 2) return 0;
    memset(&prompt, 0, sizeof(prompt));
    strncpy(prompt.title, title ? title : "", sizeof(prompt.title));
    strncpy(prompt.label, label ? label : "", sizeof(prompt.label));
    prompt.buf = buf;
    prompt.len = (int)strlen(buf);
    if (prompt.len > buflen - 1) prompt.len = buflen - 1;
    buf[prompt.len] = 0;
    prompt.max = buflen - 1;
    if (prompt.max > 27) prompt.max = 27;
    prompt.on = 1;
    prompt.result = -1;
    menu_on = 0;
    sys_on = 0;
    gui_invalidate();
    while (prompt.result < 0) gui_pump();
    prompt.on = 0;
    gui_invalidate();
    return prompt.result;
}

static void pick_geom(int *x, int *y, int *w, int *h) {
    *w = 360;
    *h = 280;
    if (*w > gfx_width() - 16) *w = gfx_width() - 16;
    if (*h > gfx_height() - 16) *h = gfx_height() - 16;
    *x = (gfx_width() - *w) / 2;
    *y = (gfx_height() - *h) / 2;
}

static void paint_pick(void) {
    int x, y, w, h;
    pick_geom(&x, &y, &w, &h);
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, TITLE_H, COL_NAVY);
    gfx_text(x + 10, y + 4, pick.title, COL_WHITE, COL_NAVY);
    int lx = x + 12, ly = y + 30, lw = w - 24, lh = h - 80;
    gfx_sunken(lx, ly, lw, lh, COL_WHITE);
    int rows = (lh - 8) / 18;
    if (rows < 1) rows = 1;
    if (pick.sel < pick.scroll) pick.scroll = pick.sel;
    if (pick.sel >= pick.scroll + rows) pick.scroll = pick.sel - rows + 1;
    for (int i = 0; i < rows; i++) {
        int idx = pick.scroll + i;
        if (idx >= pick.count) break;
        int iy = ly + 4 + i * 18;
        int hot = (idx == pick.sel);
        if (hot) gfx_fill(lx + 2, iy, lw - 4, 18, COL_NAVY);
        gfx_text(lx + 6, iy + 1, pick.names[idx], hot ? COL_WHITE : COL_BLACK,
                 hot ? COL_NAVY : COL_WHITE);
    }
    int bx[2], bw;
    char labels[2][16];
    strncpy(labels[0], "Open", 16);
    strncpy(labels[1], "Cancel", 16);
    button_row(x, y + h - 36, w, 2, labels, bx, &bw);
}

int gui_pick(const char *title, char names[][28], int count) {
    memset(&pick, 0, sizeof(pick));
    strncpy(pick.title, title ? title : "Open", sizeof(pick.title));
    if (count > 48) count = 48;
    pick.count = count;
    for (int i = 0; i < count; i++) strncpy(pick.names[i], names[i], 28);
    pick.sel = 0;
    pick.on = 1;
    pick.result = -1;
    menu_on = 0;
    sys_on = 0;
    gui_invalidate();
    while (pick.result < 0) gui_pump();
    pick.on = 0;
    gui_invalidate();
    return pick.result == 1 ? pick.sel : -1;
}

static int pick_click(int mx, int my, int *row_out) {
    int x, y, w, h;
    pick_geom(&x, &y, &w, &h);
    int bw0 = 88, gap = 10, total = 2 * bw0 + gap;
    int sx = x + (w - total) / 2;
    int by = y + h - 36;
    for (int i = 0; i < 2; i++) {
        int bx = sx + i * (bw0 + gap);
        if (mx >= bx && mx < bx + bw0 && my >= by && my < by + 24) return i == 0 ? 1 : 0;
    }
    int lx = x + 12, ly = y + 30, lw = w - 24, lh = h - 80;
    if (mx >= lx && mx < lx + lw && my >= ly && my < ly + lh) {
        int rows = (lh - 8) / 18;
        int idx = pick.scroll + (my - (ly + 4)) / 18;
        if (idx >= 0 && idx < pick.count && (my - (ly + 4)) / 18 < rows) {
            if (row_out) *row_out = idx;
            return 2;
        }
    }
    return -1;
}

static void compose(void) {
    gfx_buttons_begin();
    gfx_reset_clip();
    wall_draw();
    paint_minimized();
    Window *order[MAXW];
    int n = 0;
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && !wins[i].minimized) order[n++] = &wins[i];
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (order[j]->z < order[i]->z) {
                Window *t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (int i = 0; i < n; i++) paint_window(order[i]);
    paint_menu_dropdown();
    paint_sys_menu();
    if (dialog.on) paint_dialog();
    if (prompt.on) paint_prompt_box();
    if (pick.on) paint_pick();
    if (filedlg.on) paint_filedlg();
    gfx_present();
    cursor_invalidate();
    cursor_draw(mouse_x, mouse_y);
    dirty = 0;
}

static Window *top_at(int x, int y) {
    Window *best = 0;
    for (int i = 0; i < MAXW; i++) {
        Window *w = &wins[i];
        if (!w->used || w->minimized) continue;
        if (x < w->x || y < w->y || x >= w->x + w->w || y >= w->y + w->h) continue;
        if (!best || w->z >= best->z) best = w;
    }
    return best;
}

static int minimized_hit(int mx, int my) {
    for (int i = MAXW - 1; i >= 0; i--) {
        if (!wins[i].used || !wins[i].minimized) continue;
        int x, y;
        minimized_slot(i, &x, &y);
        if (mx >= x && mx < x + 72 && my >= y && my < y + 52) return i;
    }
    return -1;
}

static void update_menu_hover(void) {
    if (!menu_on) return;
    int x, y, wd, ht;
    dropdown_rect(menu_win, menu_i, &x, &y, &wd, &ht);
    int hover = -1;
    if (mouse_x >= x && mouse_x < x + wd && mouse_y >= y && mouse_y < y + ht) {
        hover = (mouse_y - (y + 3)) / 20;
        if (hover < 0 || hover >= menu_win->menus[menu_i].count) hover = -1;
    }
    if (hover != menu_hover) {
        menu_hover = hover;
        gui_invalidate();
    }
}

static void client_event(Window *w, int type, int dbl) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    Event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.x = mouse_x - cx;
    e.y = mouse_y - cy;
    e.dbl = dbl;
    send(w, &e);
    gui_invalidate();
}

static int delivering_click;

static void on_left_down(void) {
    int dbl = 0;
    uint32_t t = timer_ticks();
    if (t - click_tick < (uint32_t)settings_dbl_ticks() && iabs(mouse_x - click_x) < 6 && iabs(mouse_y - click_y) < 6) dbl = 1;
    click_tick = t;
    click_x = mouse_x;
    click_y = mouse_y;

    if (!delivering_click) {
        int bx, by, bw, bh;
        if (gfx_button_hit(mouse_x, mouse_y, &bx, &by, &bw, &bh)) {
            gfx_button_arm(bx, by, bw, bh);
            gui_invalidate();
            return;
        }
    }

    if (dialog.on) {
        int b = dialog_click(mouse_x, mouse_y);
        if (b) dialog.result = b;
        return;
    }
    if (prompt.on) {
        int b = prompt_click(mouse_x, mouse_y);
        if (b >= 0) prompt.result = b;
        return;
    }
    if (pick.on) {
        int row = -1;
        int b = pick_click(mouse_x, mouse_y, &row);
        if (b == 0) pick.result = 0;
        else if (b == 1) { if (pick.count) pick.result = 1; }
        else if (b == 2) {
            if (row == pick.sel && dbl) pick.result = 1;
            else { pick.sel = row; gui_invalidate(); }
        }
        return;
    }
    if (filedlg.on) {
        int row = -1;
        int b = file_click(mouse_x, mouse_y, &row, dbl);
        if (b == 0) filedlg.result = 0;
        else if (b == 1) filedlg.result = 1;
        else if (b == 2 && row >= 0) {
            filedlg.sel = row;
            if (!filedlg.isdir[row]) {
                strncpy(filedlg.name, filedlg.labels[row], 28);
                filedlg.namelen = (int)strlen(filedlg.name);
            }
            gui_invalidate();
        } else if (b == 3 && row >= 0) {
            if (filedlg.ids[row] < 0) filedlg.dir = fs_parent(filedlg.dir);
            else filedlg.dir = (uint16_t)filedlg.ids[row];
            file_refresh();
            gui_invalidate();
        }
        return;
    }
    if (sys_on && sys_win) {
        int x = sys_win->x + BORDER + 2;
        int y = sys_win->y + BORDER + TITLE_H;
        if (mouse_x >= x && mouse_x < x + 120 && mouse_y >= y && mouse_y < y + 4 * 20 + 6) {
            int item = (mouse_y - (y + 3)) / 20;
            Window *w = sys_win;
            sys_on = 0;
            if (item == 0) { if (w->maximized) toggle_max(w); else gui_restore(w); }
            else if (item == 1) { w->minimized = 1; gui_invalidate(); }
            else if (item == 2) toggle_max(w);
            else if (item == 3) request_close(w);
            return;
        }
        sys_on = 0;
    }
    if (menu_on && menu_win) {
        int x, y, wd, ht;
        dropdown_rect(menu_win, menu_i, &x, &y, &wd, &ht);
        if (mouse_x >= x && mouse_x < x + wd && mouse_y >= y && mouse_y < y + ht) {
            int item = (mouse_y - (y + 3)) / 20;
            if (item >= 0 && item < menu_win->menus[menu_i].count) {
                Event e;
                memset(&e, 0, sizeof(e));
                e.type = EVT_MENU;
                e.menu_id = menu_win->menus[menu_i].ids[item];
                Window *w = menu_win;
                menu_on = 0;
                send(w, &e);
                gui_invalidate();
                return;
            }
        }
        menu_on = 0;
        gui_invalidate();
        /* fall through so the same click can hit a title or icon */
    }

    int mi = minimized_hit(mouse_x, mouse_y);
    if (mi >= 0) {
        gui_restore(&wins[mi]);
        return;
    }

    Window *w = top_at(mouse_x, mouse_y);
    if (!w) {
        drag = 0;
        return;
    }
    int which_menu = 0;
    int hit = hit_window(w, mouse_x, mouse_y, &which_menu);
    gui_focus(w);
    if (hit == HIT_CLOSE) { request_close(w); return; }
    if (hit == HIT_MIN) { w->minimized = 1; drag = 0; gui_invalidate(); return; }
    if (hit == HIT_MAX) { toggle_max(w); return; }
    if (hit == HIT_SYS) {
        if (dbl) { request_close(w); sys_on = 0; return; }
        sys_on = 1;
        sys_win = w;
        menu_on = 0;
        gui_invalidate();
        return;
    }
    if (hit == HIT_TITLE && !w->maximized) {
        drag = 1;
        drag_win = w;
        drag_dx = mouse_x - w->x;
        drag_dy = mouse_y - w->y;
        return;
    }
    if (hit == HIT_MENU) {
        menu_on = 1;
        menu_win = w;
        menu_i = which_menu;
        menu_hover = -1;
        sys_on = 0;
        update_menu_hover();
        gui_invalidate();
        return;
    }
    if (hit == HIT_CLIENT) {
        drag = 2;
        drag_win = w;
        client_event(w, EVT_DOWN, dbl);
        return;
    }
}

static void press_focused_button(void) {
    int x, y, w, h;
    if (!gfx_button_focus_rect(&x, &y, &w, &h)) return;
    gfx_button_arm(x, y, w, h);
    compose();
    delivering_click = 1;
    int ox = mouse_x, oy = mouse_y;
    mouse_x = x + w / 2;
    mouse_y = y + h / 2;
    on_left_down();
    mouse_x = ox;
    mouse_y = oy;
    delivering_click = 0;
    gfx_button_clear_arm();
    gui_invalidate();
}

static void on_left_up(void) {
    if (gfx_button_armed()) {
        int inside = gfx_button_arm_contains(mouse_x, mouse_y);
        int x, y, w, h;
        gfx_button_focus_rect(&x, &y, &w, &h);
        gfx_button_clear_arm();
        if (inside) {
            delivering_click = 1;
            int ox = mouse_x, oy = mouse_y;
            mouse_x = x + w / 2;
            mouse_y = y + h / 2;
            on_left_down();
            mouse_x = ox;
            mouse_y = oy;
            delivering_click = 0;
        }
        gui_invalidate();
    }
    if (drag == 2 && drag_win && drag_win->used) client_event(drag_win, EVT_UP, 0);
    drag = 0;
    drag_win = 0;
}

static void on_right_down(void) {
    if (modal_active() || menu_on || sys_on) return;
    Window *w = top_at(mouse_x, mouse_y);
    if (!w) return;
    int dummy = 0;
    if (hit_window(w, mouse_x, mouse_y, &dummy) == HIT_CLIENT) {
        gui_focus(w);
        client_event(w, EVT_RDOWN, 0);
    }
}

static void on_move(void) {
    if (menu_on || gfx_button_armed()) gui_invalidate();
    if (menu_on) update_menu_hover();
    if (sys_on) gui_invalidate();
    if (drag == 1 && drag_win && drag_win->used && !drag_win->maximized) {
        drag_win->x = mouse_x - drag_dx;
        drag_win->y = mouse_y - drag_dy;
        if (drag_win->y < 0) drag_win->y = 0;
        if (drag_win->y > gfx_height() - 24) drag_win->y = gfx_height() - 24;
        if (drag_win->x < 40 - drag_win->w) drag_win->x = 40 - drag_win->w;
        if (drag_win->x > gfx_width() - 40) drag_win->x = gfx_width() - 40;
        gui_invalidate();
    } else if (drag == 2 && drag_win && drag_win->used && (mouse_buttons & 1)) {
        client_event(drag_win, EVT_MOVE, 0);
    }
}

static void cycle_focus(void) {
    if (!focused) {
        for (int i = 0; i < MAXW; i++) if (wins[i].used) { gui_restore(&wins[i]); return; }
        return;
    }
    Window *best = 0;
    Window *first = 0;
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) continue;
        if (!first || wins[i].z < first->z) first = &wins[i];
        if (wins[i].z > focused->z && (!best || wins[i].z < best->z)) best = &wins[i];
    }
    Window *n = best ? best : first;
    if (n) {
        n->minimized = 0;
        gui_focus(n);
    }
}

static void on_key(KeyEvent *k) {
    if (dialog.on) {
        if (k->code == 27) dialog.result = 0;
        else if (k->code == 10 || k->code == 13 || k->code == ' ') press_focused_button();
        return;
    }
    if (pick.on) {
        if (k->code == 27) { pick.result = 0; return; }
        if (k->code == 10 || k->code == 13 || k->code == ' ') { if (pick.count) pick.result = 1; return; }
        if (k->code == KEY_UP && pick.sel > 0) { pick.sel--; gui_invalidate(); }
        if (k->code == KEY_DOWN && pick.sel + 1 < pick.count) { pick.sel++; gui_invalidate(); }
        return;
    }
    if (filedlg.on) {
        if (k->code == 27) { filedlg.result = 0; return; }
        if (k->code == 10 || k->code == 13) {
            if (filedlg.sel >= 0 && filedlg.sel < filedlg.count && filedlg.isdir[filedlg.sel] && !filedlg.name[0]) {
                if (filedlg.ids[filedlg.sel] < 0) filedlg.dir = fs_parent(filedlg.dir);
                else filedlg.dir = (uint16_t)filedlg.ids[filedlg.sel];
                file_refresh();
                gui_invalidate();
            } else filedlg.result = 1;
            return;
        }
        if (k->code == KEY_UP && filedlg.sel > 0) { filedlg.sel--; gui_invalidate(); return; }
        if (k->code == KEY_DOWN && filedlg.sel + 1 < filedlg.count) { filedlg.sel++; gui_invalidate(); return; }
        if (k->code == 8) {
            if (filedlg.namelen > 0) filedlg.name[--filedlg.namelen] = 0;
            gui_invalidate();
            return;
        }
        if (k->code >= 32 && k->code < 127 && filedlg.namelen < 27) {
            char c = (char)k->code;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-';
            if (ok) {
                filedlg.name[filedlg.namelen++] = c;
                filedlg.name[filedlg.namelen] = 0;
                gui_invalidate();
            }
        }
        return;
    }
    if (prompt.on) {
        if (k->code == 27) { prompt.result = 0; return; }
        if (k->code == 10 || k->code == 13) { prompt.result = 1; return; }
        if (k->code == 8) {
            if (prompt.len > 0) prompt.buf[--prompt.len] = 0;
            gui_invalidate();
            return;
        }
        if (k->code >= 32 && k->code < 127 && prompt.len < prompt.max) {
            char c = (char)k->code;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-';
            if (ok) {
                prompt.buf[prompt.len++] = c;
                prompt.buf[prompt.len] = 0;
                gui_invalidate();
            }
        }
        return;
    }
    if (k->code == 27 && (menu_on || sys_on)) {
        menu_on = 0;
        sys_on = 0;
        gui_invalidate();
        return;
    }
    if (k->alt && k->code == KEY_F4) {
        if (focused) request_close(focused);
        return;
    }
    if ((k->code == ' ' || k->code == 10 || k->code == 13) &&
        !(focused && focused->text_keys) && gfx_button_focus_rect(0, 0, 0, 0)) {
        press_focused_button();
        return;
    }
    if (k->alt && k->code == 9) { cycle_focus(); return; }
    if (k->code >= KEY_F2 && k->code <= KEY_F12 && launch_fn) {
        launch_fn(k->code - KEY_F2 + 2);
        return;
    }
    if (focused && focused->used && !focused->minimized) {
        Event e;
        memset(&e, 0, sizeof(e));
        e.type = EVT_KEY;
        e.code = k->code;
        e.shift = k->shift;
        e.ctrl = k->ctrl;
        e.alt = k->alt;
        send(focused, &e);
        gui_invalidate();
    }
}

static void screensaver(void) {
    int guard = rtc_seconds_of_day();
    for (;;) {
        int t = rtc_seconds_of_day();
        gfx_reset_clip();
        gfx_clear(COL_NAVY);
        for (int i = 0; i < 7; i++) {
            int x = (t * (18 + i * 9) + i * 90) % (gfx_width() > 0 ? gfx_width() : 1);
            int y = (t * (11 + i * 7) + i * 40) % (gfx_height() > 0 ? gfx_height() : 1);
            if ((i % 3) == 0) gfx_fill_circle(x, y, 16, COL_YELLOW);
            else if ((i % 3) == 1) gfx_fill(x, y, 32, 32, COL_RED);
            else gfx_fill_triangle(x, y + 28, x + 18, y, x + 36, y + 28, COL_LTBLUE);
        }
        gfx_text(16, 16, "GeometricalOS", COL_WHITE, COL_NAVY);
        gfx_present();
        input_poll();
        MouseEvent m;
        int wake = 0;
        while (input_mouse(&m)) wake = 1;
        KeyEvent k;
        while (input_key(&k)) wake = 1;
        if (wake) break;
        int elapsed = t - guard;
        if (elapsed < 0) elapsed += 86400;
        if (elapsed > 120) break;
    }
    cursor_invalidate();
    gui_invalidate();
}

void gui_pump(void) {
    sound_tick();
    static int last_sec = -1;
    static int last_input = -1;
    int sec = rtc_seconds_of_day();
    if (last_input < 0) last_input = sec;
    if (sec != last_sec) { last_sec = sec; dirty = 1; }
    int ox = mouse_x, oy = mouse_y;
    int activity = 0;
    input_poll();
    MouseEvent m;
    while (input_mouse(&m)) {
        activity = 1;
        if (m.type == MOUSE_LDOWN) on_left_down();
        else if (m.type == MOUSE_LUP) on_left_up();
        else if (m.type == MOUSE_RDOWN) on_right_down();
    }
    KeyEvent k;
    while (input_key(&k)) { activity = 1; on_key(&k); }
    if (mouse_x != ox || mouse_y != oy) { activity = 1; on_move(); }
    if (activity) last_input = sec;
    if (!modal_active()) {
        int idle = sec - last_input;
        if (idle < 0) idle += 86400;
        if (settings_saver_on() && idle >= settings_saver_seconds()) {
            screensaver();
            last_input = rtc_seconds_of_day();
        }
    }
    if (dirty) compose();
    else if (mouse_x != ox || mouse_y != oy) {
        cursor_restore();
        cursor_draw(mouse_x, mouse_y);
    }
}

void gui_run(void) {
    gui_invalidate();
    for (;;) gui_pump();
}
