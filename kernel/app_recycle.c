#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "serial.h"
#include "io.h"

static Window *win;
static int entries[160];
static int count;
static int selected;
static int scroll;
static char message[48];

static void reload_bin(void) {
    int bin = fs_recycle_bin();
    count = bin < 0 ? 0 : fs_list((uint16_t)bin, entries, 160);
    if (selected >= count) selected = count - 1;
    if (selected < 0) selected = 0;
}

static void restore_selected(void) {
    if (selected < 0 || selected >= count) return;
    int result = fs_restore(entries[selected]);
    if (result == -2)
        gui_dialog("Restore", "An item with that name already exists in its original folder.",
                   "OK", 0, 0);
    else if (result)
        gui_dialog("Restore", "The item could not be restored.", "OK", 0, 0);
    else
        strncpy(message, "Item restored.", sizeof(message));
    reload_bin();
    gui_invalidate();
}

static void empty_bin(void) {
    if (gui_dialog("Empty Recycle Bin", "Permanently delete all items?",
                   "Empty", "Cancel", 0) != 1) return;
    if (fs_empty_recycle_bin())
        gui_dialog("Recycle Bin", "Could not empty every item.", "OK", 0, 0);
    else
        strncpy(message, "Recycle Bin emptied.", sizeof(message));
    reload_bin();
    gui_invalidate();
}

static void recycle_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int rows = (ch - 78) / 20;
    if (rows < 1) rows = 1;

    if (e->type == EVT_TIMER) {
        reload_bin();
        return;
    }

    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy, cw, 20, COL_GRAY);
        gfx_text(cx + 8, cy + 2, "Name", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 80, cy + 2, "Type", COL_BLACK, COL_GRAY);
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + rows) scroll = selected - rows + 1;
        for (int i = 0; i < rows; i++) {
            int idx = scroll + i;
            if (idx >= count) break;
            int y = cy + 22 + i * 20;
            int hot = idx == selected;
            if (hot) gfx_fill(cx + 1, y, cw - 2, 20, COL_NAVY);
            uint32_t fg = hot ? COL_WHITE : COL_BLACK;
            uint32_t bg = hot ? COL_NAVY : COL_WHITE;
            gfx_text(cx + 8, y + 2, fs_name(entries[idx]), fg, bg);
            gfx_text(cx + cw - 80, y + 2,
                     fs_is_dir(entries[idx]) ? "Folder" : "File", fg, bg);
        }
        if (!count) gfx_text(cx + 12, cy + 30, "The Recycle Bin is empty.",
                             COL_DKGRAY, COL_WHITE);
        gfx_fill(cx, cy + ch - 56, cw, 56, COL_GRAY);
        gfx_text(cx + 8, cy + ch - 52, message, COL_BLACK, COL_GRAY);
        gfx_button(cx + 8, cy + ch - 30, 104, 24, "Restore", 0);
        gfx_button(cx + 122, cy + ch - 30, 120, 24, "Empty Bin", 0);
        return;
    }

    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) restore_selected();
        else if (e->menu_id == 2) empty_bin();
        else if (e->menu_id == 3) gui_close(w);
        return;
    }
    if (e->type == EVT_RDOWN) {
        if (e->y >= 22 && e->y < ch - 56) {
            int idx = scroll + (e->y - 22) / 20;
            if (idx >= 0 && idx < count) {
                selected = idx;
                app_files_context_open(entries[idx], mouse_x, mouse_y);
            }
        }
        return;
    }
    if (e->type == EVT_DROP) {
        int result = app_files_move_inode(e->code, fs_recycle_bin());
        if (result)
            gui_dialog("Recycle Bin", "Could not move that item to the Recycle Bin.",
                       "OK", 0, 0);
        else
            reload_bin();
        gui_invalidate();
        return;
    }
    if (e->type == EVT_DOWN) {
        if (e->y >= ch - 30 && e->y < ch - 6) {
            if (e->x >= 8 && e->x < 112) restore_selected();
            else if (e->x >= 122 && e->x < 242) empty_bin();
        } else if (e->y >= 22 && e->y < ch - 56) {
            int idx = scroll + (e->y - 22) / 20;
            if (idx >= 0 && idx < count) {
                selected = idx;
                if (e->dbl) restore_selected();
            }
        }
        return;
    }
    if (e->type != EVT_KEY) return;
    if (e->code == KEY_UP && selected > 0) selected--;
    else if (e->code == KEY_DOWN && selected + 1 < count) selected++;
    else if (e->code == 10 || e->code == 13) restore_selected();
    else if (e->code == 'e' || e->code == 'E') empty_bin();
}

void app_recycle_open(void) {
    if (win) {
        gui_restore(win);
        reload_bin();
        return;
    }
    reload_bin();
    int ww = 420, wh = 320;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("Recycle Bin", ww, wh, ICON_RECYCLE, recycle_event, &win);
    if (!win) return;
    gui_set_timer(win, 100u);
    window_add_menu(win, "File");
    window_add_item(win, 0, "Restore", 1);
    window_add_item(win, 0, "Empty Recycle Bin", 2);
    window_add_item(win, 0, "Exit", 3);
    serial_write("APP: RECYCLE BIN\n");
}
