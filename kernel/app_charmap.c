#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "serial.h"

#define MAP_FIRST 32
#define MAP_COUNT 96
#define MAP_COLS 16

static Window *win;
static int selected = 'A' - MAP_FIRST;
static int clipboard_char = 'A';

int app_charmap_clipboard(void) {
    return clipboard_char;
}

static void copy_selected(void) {
    clipboard_char = MAP_FIRST + selected;
}

static void charmap_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int cell_w = (cw - 20) / MAP_COLS;
    int cell_h = 26;
    int grid_y = 32;
    if (cell_w < 16) cell_w = 16;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 10, cy + 8, "Select a character, then Copy to use it in Write.",
                 COL_BLACK, COL_GRAY);
        for (int i = 0; i < MAP_COUNT; i++) {
            int col = i % MAP_COLS, row = i / MAP_COLS;
            int x = cx + 10 + col * cell_w;
            int y = cy + grid_y + row * cell_h;
            if (i == selected) gfx_fill(x, y, cell_w - 2, cell_h - 2, COL_NAVY);
            gfx_rect(x, y, cell_w - 2, cell_h - 2, i == selected ? COL_WHITE : COL_DKGRAY);
            gfx_char(x + (cell_w - 8) / 2, y + 4, (unsigned char)(MAP_FIRST + i),
                     i == selected ? COL_WHITE : COL_BLACK,
                     i == selected ? COL_NAVY : COL_GRAY, 1);
        }
        gfx_text(cx + 12, cy + ch - 34, "Selected:", COL_BLACK, COL_GRAY);
        gfx_char(cx + 84, cy + ch - 36, (unsigned char)(MAP_FIRST + selected),
                 COL_BLACK, COL_GRAY, 1);
        gfx_button(cx + cw - 184, cy + ch - 38, 78, 28, "Copy", 0);
        gfx_button(cx + cw - 96, cy + ch - 38, 84, 28, "Close", 0);
        return;
    }
    if (e->type == EVT_DOWN) {
        if (e->y >= ch - 40) {
            if (e->x >= cw - 184 && e->x < cw - 106) {
                copy_selected();
                gui_dialog("Character Map", "Character copied. Use Ctrl+V in Write.", "OK", 0, 0);
            } else if (e->x >= cw - 96) gui_close(w);
            return;
        }
        int x = e->x - 10, y = e->y - grid_y;
        int col = x / cell_w, row = y / cell_h;
        int index = row * MAP_COLS + col;
        if (x >= 0 && y >= 0 && col >= 0 && col < MAP_COLS &&
            row >= 0 && index < MAP_COUNT) {
            selected = index;
            if (e->dbl) copy_selected();
        }
        return;
    }
    if (e->type != EVT_KEY) return;
    if (e->code == KEY_LEFT && selected % MAP_COLS > 0) selected--;
    else if (e->code == KEY_RIGHT && selected % MAP_COLS < MAP_COLS - 1 &&
             selected + 1 < MAP_COUNT) selected++;
    else if (e->code == KEY_UP && selected >= MAP_COLS) selected -= MAP_COLS;
    else if (e->code == KEY_DOWN && selected + MAP_COLS < MAP_COUNT) selected += MAP_COLS;
    else if (e->code == 10 || e->code == 13 || e->code == 'c' || e->code == 'C') {
        copy_selected();
        gui_invalidate();
        return;
    }
}

void app_charmap_open(void) {
    if (win) { gui_restore(win); return; }
    int ww = 460, wh = 280;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("Character Map", ww, wh, ICON_CHARMAP, charmap_event, &win);
    if (win) win->text_keys = 0;
    serial_write("APP: CHARACTER MAP\n");
}
