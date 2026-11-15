#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "io.h"
#include "serial.h"

static Window *win;
static uint16_t cwd = FS_ROOT;
static int ids[160];
static int count, sel, scroll;

static void reload(void) {
    count = fs_list(cwd, ids, 160);
    int total = count + (cwd != FS_ROOT);
    if (total <= 0) sel = 0;
    else if (sel >= total) sel = total - 1;
    if (sel < 0) sel = 0;
}

static void open_sel(void) {
    int extra = (cwd != FS_ROOT);
    int total = count + extra;
    if (sel < 0 || sel >= total) return;
    if (extra && sel == 0) {
        cwd = fs_parent(cwd);
        sel = 0;
        scroll = 0;
        reload();
        return;
    }
    int id = ids[sel - extra];
    if (fs_is_dir(id)) {
        cwd = (uint16_t)id;
        sel = 0;
        scroll = 0;
        reload();
        return;
    }
    const char *name = fs_name(id);
    int n = (int)strlen(name);
    int txt = n >= 4 && strcasecmp(name + n - 4, ".TXT") == 0;
    int gim = n >= 4 && strcasecmp(name + n - 4, ".GIM") == 0;
    if (txt) app_notepad_load(id);
    else if (gim) app_paint_load(id);
    else gui_dialog("File Manager", "Open .TXT in Notepad or .GIM in Paint.", "OK", 0, 0);
}

static void files_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int rows = (ch - 28) / 18;
    if (rows < 1) rows = 1;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy, cw, 18, COL_GRAY);
        gfx_text(cx + 8, cy + 1, "Name", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 140, cy + 1, "Size", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 70, cy + 1, "Type", COL_BLACK, COL_GRAY);
        gfx_hline(cx, cy + 18, cw, COL_WHITE);
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows) scroll = sel - rows + 1;
        int shown = 0;
        int extra = (cwd != FS_ROOT);
        int total = count + extra;
        for (int i = 0; i < rows; i++) {
            int idx = scroll + i;
            if (idx >= total) break;
            int y = cy + 20 + i * 18;
            int hot = (idx == sel);
            if (hot) gfx_fill(cx + 1, y, cw - 2, 18, COL_NAVY);
            uint32_t fg = hot ? COL_WHITE : COL_BLACK;
            uint32_t bg = hot ? COL_NAVY : COL_WHITE;
            if (extra && idx == 0) {
                gfx_text(cx + 8, y + 1, "..", fg, bg);
                gfx_text(cx + cw - 70, y + 1, "Folder", fg, bg);
            } else {
                int id = ids[idx - extra];
                gfx_text(cx + 8, y + 1, fs_name(id), fg, bg);
                if (fs_is_dir(id)) {
                    gfx_text(cx + cw - 70, y + 1, "Folder", fg, bg);
                } else {
                    char sz[16];
                    uint32_t s = fs_size(id);
                    int n = 0;
                    if (s == 0) sz[n++] = '0';
                    else {
                        char rev[16]; int r = 0;
                        while (s && r < 12) { rev[r++] = (char)('0' + s % 10); s /= 10; }
                        while (r) sz[n++] = rev[--r];
                    }
                    sz[n] = 0;
                    gfx_text(cx + cw - 140, y + 1, sz, fg, bg);
                    gfx_text(cx + cw - 70, y + 1, "File", fg, bg);
                }
            }
            shown++;
        }
        (void)shown;
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) open_sel();
        else if (e->menu_id == 2) {
            if (cwd != FS_ROOT) {
                cwd = fs_parent(cwd);
                sel = 0; scroll = 0; reload();
            }
        }         else if (e->menu_id == 3) {
            int extra = (cwd != FS_ROOT);
            int total = count + extra;
            if (sel < 0 || sel >= total || (extra && sel == 0)) return;
            int id = ids[sel - extra];
            if (gui_dialog("Delete", "Delete the selected item?", "Delete", "Cancel", 0) == 1) {
                if (fs_delete(id))
                    gui_dialog("File Manager", "Could not delete that item.\nFolders must be empty.", "OK", 0, 0);
                reload();
            }
        } else if (e->menu_id == 4) gui_close(w);
        return;
    }
    int extra = (cwd != FS_ROOT);
    int total = count + extra;
    if (e->type == EVT_DOWN) {
        int idx = scroll + (e->y - 20) / 18;
        if (e->y >= 20 && idx >= 0 && idx < total) {
            if (idx == sel && e->dbl) open_sel();
            else sel = idx;
        }
        return;
    }
    if (e->type == EVT_KEY) {
        if (e->code == KEY_UP && sel > 0) sel--;
        else if (e->code == KEY_DOWN && sel + 1 < total) sel++;
        else if (e->code == 10 || e->code == 13) open_sel();
        else if (e->code == 8) {
            if (cwd != FS_ROOT) { cwd = fs_parent(cwd); sel = 0; scroll = 0; reload(); }
        }
    }
}

void app_files_open(void) {
    if (win) { gui_restore(win); reload(); return; }
    reload();
    int ww = 480, wh = 360;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("File Manager", ww, wh, ICON_FILES, files_event, &win);
    if (!win) return;
    window_add_menu(win, "File");
    window_add_item(win, 0, "Open", 1);
    window_add_item(win, 0, "Up", 2);
    window_add_item(win, 0, "Delete", 3);
    window_add_item(win, 0, "Exit", 4);
    serial_write("APP: FILES\n");
}
