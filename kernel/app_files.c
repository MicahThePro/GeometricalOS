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
static int clipboard_inode = -1;
static int clipboard_cut;
static char status[48];

static void reload(void) {
    count = fs_list(cwd, ids, 160);
    if (cwd == FS_ROOT) {
        int visible = 0;
        for (int i = 0; i < count; i++)
            if (strcasecmp(fs_name(ids[i]), ".GOS") != 0)
                ids[visible++] = ids[i];
        count = visible;
    }
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
        status[0] = 0;
        reload();
        return;
    }
    int id = ids[sel - extra];
    if (fs_is_dir(id)) {
        cwd = (uint16_t)id;
        sel = 0;
        scroll = 0;
        status[0] = 0;
        reload();
        return;
    }
    if (!app_files_open_inode(id))
        gui_dialog("File Manager",
                   "Open .TXT in Notepad, .WRI in Write,\n.CRD in Cardfile, or .GIM in Paint.",
                   "OK", 0, 0);
}

int app_files_open_inode(int id) {
    if (id <= 0 || !fs_type(id) || fs_is_dir(id)) return 0;
    const char *name = fs_name(id);
    int n = (int)strlen(name);
    int txt = n >= 4 && strcasecmp(name + n - 4, ".TXT") == 0;
    int gim = n >= 4 && strcasecmp(name + n - 4, ".GIM") == 0;
    int wri = n >= 4 && strcasecmp(name + n - 4, ".WRI") == 0;
    int crd = n >= 4 && strcasecmp(name + n - 4, ".CRD") == 0;
    if (txt) app_notepad_load(id);
    else if (gim) app_paint_load(id);
    else if (wri) app_write_load(id);
    else if (crd) app_cardfile_load(id);
    else return 0;
    return 1;
}

static int selected_file(void) {
    int extra = cwd != FS_ROOT;
    int total = count + extra;
    if (sel < 0 || sel >= total || (extra && sel == 0)) return -1;
    return ids[sel - extra];
}

static void rename_selected(void) {
    int id = selected_file();
    if (id < 0) return;
    char name[28];
    strncpy(name, fs_name(id), sizeof(name));
    name[sizeof(name) - 1] = 0;
    if (!gui_prompt("Rename", "New name:", name, sizeof(name))) return;
    if (fs_rename(id, name)) {
        gui_dialog("File Manager",
                   "Could not rename. Check the name and\nmake sure it is not already in use.",
                   "OK", 0, 0);
        return;
    }
    strncpy(status, "Item renamed.", sizeof(status));
    reload();
}

static void copy_selected(int cut) {
    int id = selected_file();
    if (id < 0) return;
    clipboard_inode = id;
    clipboard_cut = cut;
    strncpy(status, cut ? "Cut. Choose a folder and Paste." :
                          "Copied. Choose a folder and Paste.",
            sizeof(status));
}

static void paste_item(void) {
    if (clipboard_inode < 0 || !fs_type(clipboard_inode)) {
        gui_dialog("File Manager", "There is no valid item on the clipboard.",
                   "OK", 0, 0);
        clipboard_inode = -1;
        return;
    }
    int result;
    if (clipboard_cut) {
        result = fs_move(clipboard_inode, cwd);
    } else if (fs_lookup(cwd, fs_name(clipboard_inode)) >= 0) {
        char name[28] = "Copy of ";
        strncpy(name + 8, fs_name(clipboard_inode), sizeof(name) - 8);
        if (!gui_prompt("Copy", "Name for the copied item:", name, sizeof(name)))
            return;
        result = fs_copy_as(clipboard_inode, cwd, name);
    } else {
        result = fs_copy(clipboard_inode, cwd);
    }
    if (result) {
        gui_dialog("File Manager",
                   "Could not paste. The name may already exist,\n"
                   "the folder may be invalid, or the disk may be full.",
                   "OK", 0, 0);
        return;
    }
    strncpy(status, clipboard_cut ? "Item moved." : "Item copied.", sizeof(status));
    if (clipboard_cut) clipboard_inode = -1;
    reload();
}

static void files_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int rows = (ch - 48) / 18;
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
        }
        gfx_fill(cx, cy + ch - 20, cw, 20, COL_GRAY);
        if (status[0]) gfx_text(cx + 8, cy + ch - 18, status, COL_BLACK, COL_GRAY);
        else gfx_text(cx + 8, cy + ch - 18,
                      cwd == FS_ROOT ? "Folder: root" : fs_name(cwd),
                      COL_BLACK, COL_GRAY);
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) open_sel();
        else if (e->menu_id == 2) {
            if (cwd != FS_ROOT) {
                cwd = fs_parent(cwd);
                sel = 0; scroll = 0; status[0] = 0; reload();
            }
        } else if (e->menu_id == 3) rename_selected();
        else if (e->menu_id == 4) {
            int extra = (cwd != FS_ROOT);
            int total = count + extra;
            if (sel < 0 || sel >= total || (extra && sel == 0)) return;
            int id = ids[sel - extra];
            int in_bin = cwd == fs_recycle_bin();
            if (gui_dialog(in_bin ? "Delete permanently" : "Move to Recycle Bin",
                           in_bin ? "Permanently delete the selected item?" :
                                    "Move the selected item to the Recycle Bin?",
                           in_bin ? "Delete" : "Move", "Cancel", 0) == 1) {
                int result = in_bin ? fs_delete(id) : fs_recycle(id);
                if (result)
                    gui_dialog("File Manager",
                               in_bin ? "Could not permanently delete that item.\nFolders must be empty." :
                                        "Could not move that item to the Recycle Bin.",
                               "OK", 0, 0);
                else if (clipboard_inode == id) clipboard_inode = -1;
                reload();
            }
        } else if (e->menu_id == 5) gui_close(w);
        else if (e->menu_id == 10) copy_selected(1);
        else if (e->menu_id == 11) copy_selected(0);
        else if (e->menu_id == 12) paste_item();
        else if (e->menu_id == 20) app_help_open();
        return;
    }
    int extra = (cwd != FS_ROOT);
    int total = count + extra;
    if (e->type == EVT_DOWN) {
        int idx = scroll + (e->y - 20) / 18;
        if (e->y >= 20 && e->y < ch - 22 && idx >= 0 && idx < total) {
            if (idx == sel && e->dbl) open_sel();
            else sel = idx;
        }
        return;
    }
    if (e->type == EVT_KEY) {
        if (e->ctrl && (e->code == 'x' || e->code == 'X')) copy_selected(1);
        else if (e->ctrl && (e->code == 'c' || e->code == 'C')) copy_selected(0);
        else if (e->ctrl && (e->code == 'v' || e->code == 'V')) paste_item();
        else if (e->code == KEY_F2) rename_selected();
        else if (e->code == KEY_UP && sel > 0) sel--;
        else if (e->code == KEY_DOWN && sel + 1 < total) sel++;
        else if (e->code == 10 || e->code == 13) open_sel();
        else if (e->code == 8) {
            if (cwd != FS_ROOT) {
                cwd = fs_parent(cwd);
                sel = 0;
                scroll = 0;
                status[0] = 0;
                reload();
            }
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
    window_add_item(win, 0, "Rename", 3);
    window_add_item(win, 0, "Delete", 4);
    window_add_item(win, 0, "Exit", 5);
    window_add_menu(win, "Edit");
    window_add_item(win, 1, "Cut", 10);
    window_add_item(win, 1, "Copy", 11);
    window_add_item(win, 1, "Paste", 12);
    window_add_menu(win, "Help");
    window_add_item(win, 2, "File Manager Help", 20);
    serial_write("APP: FILES\n");
}
