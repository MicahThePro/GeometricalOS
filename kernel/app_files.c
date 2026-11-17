#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "io.h"
#include "serial.h"

#define FILE_MANAGER_REFRESH_TICKS 100u

static Window *win;
static uint16_t cwd = FS_ROOT;
static int ids[160];
static int count, sel, scroll;
static int clipboard_inode = -1;
static int clipboard_cut;
static char status[48];
static Window *context_win;
static int context_inode = -1;
static uint16_t context_dir = FS_ROOT;
static int context_ids[8];
static char context_labels[8][28];
static int context_count;
static int drag_candidate = -1;
static int drag_start_x, drag_start_y;
static int drag_started;

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
                   "Open .TXT, .WRI, .CRD, .GIM, or image files\n(PNG, JPG, JPEG, WEBP) from File Manager.",
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
    int png = n >= 4 && strcasecmp(name + n - 4, ".PNG") == 0;
    int jpg = n >= 4 && strcasecmp(name + n - 4, ".JPG") == 0;
    int jpeg = n >= 5 && strcasecmp(name + n - 5, ".JPEG") == 0;
    int webp = n >= 5 && strcasecmp(name + n - 5, ".WEBP") == 0;
    if (txt) app_notepad_load(id);
    else if (gim) app_paint_load(id);
    else if (wri) app_write_load(id);
    else if (crd) app_cardfile_load(id);
    else if (png || jpg || jpeg || webp) app_image_load(id);
    else return 0;
    return 1;
}

static int selected_file(void) {
    int extra = cwd != FS_ROOT;
    int total = count + extra;
    if (sel < 0 || sel >= total || (extra && sel == 0)) return -1;
    return ids[sel - extra];
}

static void rename_inode(int id) {
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
    gui_invalidate();
}

static void rename_selected(void) {
    rename_inode(selected_file());
}

static void copy_inode(int id, int cut) {
    if (id < 0) return;
    clipboard_inode = id;
    clipboard_cut = cut;
    strncpy(status, cut ? "Cut. Choose a folder and Paste." :
                          "Copied. Choose a folder and Paste.",
            sizeof(status));
}

static void copy_selected(int cut) {
    copy_inode(selected_file(), cut);
}

static int allocate_desktop_cell(int except_inode) {
    int desktop = desktop_files_dir();
    int capacity = desktop_file_capacity();
    if (desktop < 0) return -1;
    int items[160];
    int item_count = fs_list((uint16_t)desktop, items, 160);
    for (int cell = 0; cell < capacity; cell++) {
        int occupied = 0;
        for (int i = 0; i < item_count; i++) {
            if (items[i] != except_inode && fs_desktop_cell(items[i]) == cell) {
                occupied = 1;
                break;
            }
        }
        if (!occupied) return cell;
    }
    return -1;
}

static int ensure_desktop_cell(int inode) {
    int desktop = desktop_files_dir();
    if (desktop < 0 || fs_parent(inode) != (uint16_t)desktop) return 0;
    int cell = fs_desktop_cell(inode);
    if (cell >= 0 && cell < desktop_file_capacity()) return 0;
    cell = allocate_desktop_cell(inode);
    return cell < 0 ? -1 : fs_set_desktop_cell(inode, cell);
}

int app_files_move_inode(int inode, int destination) {
    if (inode <= 0 || !fs_type(inode) || destination < 0 ||
        !fs_is_dir(destination))
        return -1;
    int bin = fs_recycle_bin();
    if (destination == bin)
        return fs_parent(inode) == (uint16_t)bin ? 0 : fs_recycle(inode);
    int desktop = desktop_files_dir();
    if (destination == desktop &&
        fs_parent(inode) != (uint16_t)desktop &&
        allocate_desktop_cell(inode) < 0)
        return -1;
    int result;
    if (fs_parent(inode) == (uint16_t)bin) {
        result = fs_restore(inode);
        if (result) return result;
        result = fs_move(inode, (uint16_t)destination);
        if (result && fs_recycle(inode))
            serial_write("FILES: could not return failed move to Recycle Bin\n");
    } else {
        result = fs_move(inode, (uint16_t)destination);
    }
    if (!result && destination == desktop)
        result = ensure_desktop_cell(inode);
    return result;
}

static void paste_item_to(uint16_t destination) {
    if (clipboard_inode < 0 || !fs_type(clipboard_inode)) {
        gui_dialog("File Manager", "There is no valid item on the clipboard.",
                   "OK", 0, 0);
        clipboard_inode = -1;
        return;
    }
    int desktop = desktop_files_dir();
    if (desktop >= 0 && destination == (uint16_t)desktop &&
        !(clipboard_cut && fs_parent(clipboard_inode) == destination)) {
        int items[160];
        if (fs_list(destination, items, 160) >= desktop_file_capacity()) {
            gui_dialog("File Manager",
                       "The Desktop is full. Move or delete an item first.",
                       "OK", 0, 0);
            return;
        }
    }
    int result;
    if (clipboard_cut) {
        result = app_files_move_inode(clipboard_inode, destination);
    } else if (fs_lookup(destination, fs_name(clipboard_inode)) >= 0) {
        char name[28] = "Copy of ";
        strncpy(name + 8, fs_name(clipboard_inode), sizeof(name) - 8);
        if (!gui_prompt("Copy", "Name for the copied item:", name, sizeof(name)))
            return;
        result = fs_copy_as(clipboard_inode, destination, name);
    } else {
        result = fs_copy(clipboard_inode, destination);
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
    if (destination == cwd) reload();
    gui_invalidate();
}

static void paste_item(void) {
    paste_item_to(cwd);
}

static void delete_inode(int id) {
    if (id < 0) return;
    int in_bin = fs_parent(id) == (uint16_t)fs_recycle_bin();
    if (gui_dialog(in_bin ? "Delete permanently" : "Move to Recycle Bin",
                   in_bin ? "Permanently delete the selected item?" :
                            "Move the selected item to the Recycle Bin?",
                   in_bin ? "Delete" : "Move", "Cancel", 0) != 1)
        return;
    int result = in_bin ? fs_delete(id) : fs_recycle(id);
    if (result) {
        gui_dialog("File Manager",
                   in_bin ? "Could not permanently delete that item.\nFolders must be empty." :
                            "Could not move that item to the Recycle Bin.",
                   "OK", 0, 0);
        return;
    }
    if (clipboard_inode == id) clipboard_inode = -1;
    strncpy(status, in_bin ? "Item permanently deleted." : "Item moved to Recycle Bin.",
            sizeof(status));
    reload();
    gui_invalidate();
}

static void open_inode(int id) {
    if (id <= 0 || !fs_type(id)) return;
    if (fs_is_dir(id)) app_files_open_directory(id);
    else if (!app_files_open_inode(id))
        gui_dialog("File Manager",
                   "Open .TXT, .WRI, .CRD, .GIM, or image files\n(PNG, JPG, JPEG, WEBP).",
                   "OK", 0, 0);
}

enum {
    FILE_ACTION_OPEN = 1,
    FILE_ACTION_RENAME,
    FILE_ACTION_DELETE,
    FILE_ACTION_DESKTOP,
    FILE_ACTION_RESTORE,
    FILE_ACTION_CUT,
    FILE_ACTION_COPY,
    FILE_ACTION_PASTE
};

static void run_context_action(int action, int inode, uint16_t destination) {
    if (action == FILE_ACTION_OPEN) open_inode(inode);
    else if (action == FILE_ACTION_RENAME) rename_inode(inode);
    else if (action == FILE_ACTION_DELETE) delete_inode(inode);
    else if (action == FILE_ACTION_DESKTOP) {
        int dir = desktop_files_dir();
        int items[160];
        int full = dir >= 0 && fs_parent(inode) != (uint16_t)dir &&
                   fs_list((uint16_t)dir, items, 160) >= desktop_file_capacity();
        int result = dir < 0 || full ? -1 : app_files_move_inode(inode, dir);
        if (result)
            gui_dialog("File Manager",
                       full ? "The Desktop is full. Move or delete an item first." :
                              "Could not move that item to the Desktop.",
                       "OK", 0, 0);
        else {
            strncpy(status, "Item moved to Desktop.", sizeof(status));
            reload();
            gui_invalidate();
        }
    } else if (action == FILE_ACTION_RESTORE) {
        int result = fs_restore(inode);
        if (result)
            gui_dialog("File Manager", "Could not restore that item.",
                       "OK", 0, 0);
        else {
            strncpy(status, "Item restored.", sizeof(status));
            reload();
            gui_invalidate();
        }
    } else if (action == FILE_ACTION_CUT) copy_inode(inode, 1);
    else if (action == FILE_ACTION_COPY) copy_inode(inode, 0);
    else if (action == FILE_ACTION_PASTE) paste_item_to(destination);
}

static void context_add(const char *label, int action) {
    if (context_count >= 8) return;
    strncpy(context_labels[context_count], label,
            sizeof(context_labels[context_count]));
    context_ids[context_count++] = action;
}

static void context_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        for (int i = 0; i < context_count; i++)
            gfx_button(cx + 4, cy + 3 + i * 22, cw - 8, 20,
                       context_labels[i], 0);
        return;
    }
    if (e->type != EVT_DOWN || e->x < 4 || e->x >= cw - 4 || e->y < 3)
        return;
    int row = (e->y - 3) / 22;
    if (row < 0 || row >= context_count ||
        e->y >= 3 + row * 22 + 20)
        return;
    int action = context_ids[row];
    int inode = context_inode;
    uint16_t destination = context_dir;
    gui_close(w);
    run_context_action(action, inode, destination);
}

void app_files_context_open(int inode, int x, int y) {
    if (context_win) gui_close(context_win);
    context_count = 0;
    context_inode = inode;
    int bin = fs_recycle_bin();
    context_dir = cwd;
    if (inode > 0 && fs_type(inode)) {
        context_dir = fs_is_dir(inode) ? (uint16_t)inode : fs_parent(inode);
        context_add("Open", FILE_ACTION_OPEN);
        if (inode != bin) {
            context_add("Rename", FILE_ACTION_RENAME);
            context_add("Delete", FILE_ACTION_DELETE);
            if (fs_parent(inode) == (uint16_t)bin)
                context_add("Restore", FILE_ACTION_RESTORE);
            else if (fs_parent(inode) != (uint16_t)desktop_files_dir())
                context_add("Move to Desktop", FILE_ACTION_DESKTOP);
            context_add("Cut", FILE_ACTION_CUT);
            context_add("Copy", FILE_ACTION_COPY);
        }
    }
    if (clipboard_inode >= 0 && fs_type(clipboard_inode) &&
        context_dir != (uint16_t)bin)
        context_add("Paste Here", FILE_ACTION_PASTE);
    if (!context_count) return;

    int width = 190;
    int height = 28 + context_count * 22;
    context_win = gui_create("File Actions", width, height, 0,
                             context_event, &context_win);
    if (!context_win) {
        context_count = 0;
        return;
    }
    if (x + width > gfx_width()) x = gfx_width() - width;
    if (y + height > gfx_height() - GUI_TASKBAR_HEIGHT)
        y = gfx_height() - GUI_TASKBAR_HEIGHT - height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    gui_move(context_win, x, y);
}

void app_files_open_directory(int dir) {
    if (!fs_is_dir(dir)) return;
    cwd = (uint16_t)dir;
    sel = 0;
    scroll = 0;
    status[0] = 0;
    app_files_open();
    reload();
    gui_invalidate();
}

static void drop_item(int inode, int x, int y, int ch) {
    int extra = cwd != FS_ROOT;
    int total = count + extra;
    int destination = cwd;
    if (y >= 20 && y < ch - 22) {
        int idx = scroll + (y - 20) / 18;
        if (idx >= 0 && idx < total) {
            if (extra && idx == 0) {
                destination = fs_parent(cwd);
            } else {
                int target = ids[idx - extra];
                if (target == inode) return;
                if (target == fs_recycle_bin()) {
                    destination = target;
                } else if (fs_is_dir(target)) {
                    destination = target;
                } else {
                    gui_dialog("File Manager",
                               "Drop files onto a folder or an empty part of the list.",
                               "OK", 0, 0);
                    return;
                }
            }
        }
    } else if (y < 20 || y >= ch - 22) {
        return;
    }

    int result = app_files_move_inode(inode, destination);
    if (result) {
        gui_dialog("File Manager",
                   "Could not move that item. Check the destination\nand make sure its name is not already in use.",
                   "OK", 0, 0);
        return;
    }
    strncpy(status, "Item moved.", sizeof(status));
    reload();
    gui_invalidate();
}

static void files_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int rows = (ch - 48) / 18;
    if (rows < 1) rows = 1;
    int extra = cwd != FS_ROOT;
    int total = count + extra;
    if (e->type == EVT_TIMER) {
        reload();
        return;
    }
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy, cw, 18, COL_GRAY);
        gfx_text(cx + 8, cy + 1, "Name", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 140, cy + 1, "Size", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 70, cy + 1, "Type", COL_BLACK, COL_GRAY);
        gfx_hline(cx, cy + 18, cw, COL_WHITE);
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows) scroll = sel - rows + 1;
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
            delete_inode(selected_file());
        } else if (e->menu_id == 5) gui_close(w);
        else if (e->menu_id == 10) copy_selected(1);
        else if (e->menu_id == 11) copy_selected(0);
        else if (e->menu_id == 12) paste_item();
        else if (e->menu_id == 20) app_help_open();
        return;
    }
    if (e->type == EVT_RDOWN) {
        int idx = scroll + (e->y - 20) / 18;
        int id = -1;
        if (e->y >= 20 && e->y < ch - 22 &&
            idx >= 0 && idx < total &&
            !(extra && idx == 0)) {
            sel = idx;
            id = ids[idx - extra];
        }
        drag_candidate = -1;
        app_files_context_open(id, mouse_x, mouse_y);
        return;
    }
    if (e->type == EVT_DROP) {
        drop_item(e->code, e->x, e->y, ch);
        return;
    }
    if (e->type == EVT_DOWN) {
        drag_candidate = -1;
        drag_started = 0;
        int idx = scroll + (e->y - 20) / 18;
        if (e->y >= 20 && e->y < ch - 22 && idx >= 0 && idx < total) {
            if (idx == sel && e->dbl) open_sel();
            else {
                sel = idx;
                if (!(extra && idx == 0)) {
                    drag_candidate = ids[idx - extra];
                    drag_start_x = mouse_x;
                    drag_start_y = mouse_y;
                }
            }
        }
        return;
    }
    if (e->type == EVT_MOVE) {
        if (drag_candidate > 0 && !drag_started) {
            int dx = mouse_x - drag_start_x;
            int dy = mouse_y - drag_start_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx >= 5 || dy >= 5) {
                drag_started = 1;
                gui_set_drag_payload(drag_candidate);
            }
        }
        return;
    }
    if (e->type == EVT_UP) {
        drag_candidate = -1;
        drag_started = 0;
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
    gui_set_timer(win, FILE_MANAGER_REFRESH_TICKS);
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
