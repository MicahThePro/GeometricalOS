#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "io.h"
#include "serial.h"

#define NOTE_MAX 20000

static char *textb;
static int text_len, cursor, scroll;
static char filename[28];
static uint16_t parent;
static Window *win;
static int dirty;
static int line_at[1024];
static int nlines;

static void ensure_buf(void) {
    if (!textb) {
        textb = (char *)0x660000;
        textb[0] = 0;
    }
}

static void reflow(int cols) {
    if (cols < 1) cols = 1;
    nlines = 0;
    int i = 0;
    if (text_len == 0) {
        line_at[nlines++] = 0;
        return;
    }
    while (i < text_len && nlines < 1022) {
        line_at[nlines++] = i;
        int c = 0;
        while (i < text_len && textb[i] != '\n' && c < cols) { i++; c++; }
        if (i < text_len && textb[i] == '\n') i++;
    }
    if (textb[text_len - 1] == '\n' && nlines < 1023)
        line_at[nlines++] = text_len;
}

static void cursor_line_col(int cols, int *line, int *col) {
    reflow(cols);
    *line = 0;
    *col = 0;
    for (int i = 0; i < nlines; i++) {
        int start = line_at[i];
        int next = (i + 1 < nlines) ? line_at[i + 1] : text_len + 1;
        if (cursor >= start && cursor < next) {
            *line = i;
            *col = cursor - start;
            return;
        }
    }
    if (nlines) {
        *line = nlines - 1;
        *col = cursor - line_at[*line];
        if (*col < 0) *col = 0;
    }
}

static int line_end_col(int line) {
    int start = line_at[line];
    int end = (line + 1 < nlines) ? line_at[line + 1] : text_len;
    if (end > start && textb[end - 1] == '\n') end--;
    return end - start;
}

static void insert_ch(char c) {
    ensure_buf();
    if (text_len >= NOTE_MAX - 1) return;
    memmove(textb + cursor + 1, textb + cursor, (size_t)(text_len - cursor + 1));
    textb[cursor++] = c;
    text_len++;
}

static void backspace(void) {
    if (cursor <= 0) return;
    memmove(textb + cursor - 1, textb + cursor, (size_t)(text_len - cursor + 1));
    cursor--;
    text_len--;
}

static void delete_ch(void) {
    if (cursor >= text_len) return;
    memmove(textb + cursor, textb + cursor + 1, (size_t)(text_len - cursor));
    text_len--;
}

static void set_title(void) {
    char t[48];
    if (filename[0]) {
        strcpy(t, filename);
        strncpy(t + strlen(t), " - Notepad", 48 - strlen(t));
    } else {
        strcpy(t, "Untitled - Notepad");
    }
    if (win) strncpy(win->title, t, sizeof(win->title));
}

static int do_save(void) {
    ensure_buf();
    if (!filename[0]) {
        char name[28] = "NOTES.TXT";
        uint16_t dir = parent;
        if (!gui_file("Save As", 1, &dir, name, 28)) return 0;
        strncpy(filename, name, 28);
        parent = dir;
    }
    if (fs_write(parent, filename, textb, (uint32_t)text_len) < 0) {
        gui_dialog("Notepad", "Could not save the file.", "OK", 0, 0);
        return 0;
    }
    dirty = 0;
    set_title();
    serial_write("NOTE: saved ");
    serial_write(filename);
    serial_write("\n");
    return 1;
}

static int confirm_close(void) {
    if (!dirty) return 1;
    int r = gui_dialog("Notepad", "Save changes to this document?", "Yes", "No", "Cancel");
    if (r == 1) return do_save();
    if (r == 2) return 1;
    return 0;
}

static int notepad_ask(Window *w) {
    (void)w;
    return confirm_close();
}

static void do_open(void) {
    if (!confirm_close()) return;
    uint16_t dir = parent;
    char name[28] = "";
    if (!gui_file("Open", 0, &dir, name, 28)) return;
    int id = fs_lookup(dir, name);
    if (id < 0) {
        gui_dialog("Open", "That file is not in this folder.", "OK", 0, 0);
        return;
    }
    app_notepad_load(id);
}

static void notepad_event(Window *w, Event *e) {
    ensure_buf();
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int cols = cw / 8; if (cols < 1) cols = 1;
    int rows = ch / gfx_font_h(); if (rows < 1) rows = 1;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        int line, col;
        cursor_line_col(cols, &line, &col);
        if (line < scroll) scroll = line;
        if (line >= scroll + rows) scroll = line - rows + 1;
        for (int r = 0; r < rows; r++) {
            int li = scroll + r;
            if (li >= nlines) break;
            int start = line_at[li];
            int end = (li + 1 < nlines) ? line_at[li + 1] : text_len;
            int x = cx;
            for (int i = start; i < end && textb[i] != '\n'; i++) {
                gfx_char(x, cy + r * gfx_font_h(), (unsigned char)textb[i], COL_BLACK, COL_WHITE, 1);
                x += 8;
                if (x > cx + cw - 8) break;
            }
        }
        if (w == 0) return;
        if ((timer_ticks() / 8) & 1) {
            int cy0 = cy + (line - scroll) * gfx_font_h();
            gfx_fill(cx + col * 8, cy0, 2, gfx_font_h(), COL_BLACK);
        }
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) {
            if (!confirm_close()) return;
            text_len = 0; textb[0] = 0; cursor = 0; scroll = 0; filename[0] = 0; parent = FS_ROOT;
            dirty = 0;
            set_title();
        } else if (e->menu_id == 2) do_open();
        else if (e->menu_id == 3) do_save();
        else if (e->menu_id == 4) {
            char name[28];
            strncpy(name, filename[0] ? filename : "NOTES.TXT", 28);
            uint16_t dir = parent;
            if (!gui_file("Save As", 1, &dir, name, 28)) return;
            strncpy(filename, name, 28);
            parent = dir;
            do_save();
        } else if (e->menu_id == 5) {
            if (confirm_close()) gui_close(w);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        int cols2 = cols;
        reflow(cols2);
        int li = scroll + e->y / gfx_font_h();
        int col = e->x / 8;
        if (li < 0) li = 0;
        if (li >= nlines) li = nlines ? nlines - 1 : 0;
        if (nlines == 0) { cursor = 0; return; }
        int maxc = line_end_col(li);
        if (col < 0) col = 0;
        if (col > maxc) col = maxc;
        cursor = line_at[li] + col;
        if (cursor > text_len) cursor = text_len;
        return;
    }
    if (e->type != EVT_KEY) return;
    int line, col;
    cursor_line_col(cols, &line, &col);
    int code = e->code;
    if (code == 8) { backspace(); dirty = 1; }
    else if (code == KEY_DEL) { delete_ch(); dirty = 1; }
    else if (code == 10 || code == 13) { insert_ch('\n'); dirty = 1; }
    else if (code == KEY_LEFT) { if (cursor > 0) cursor--; }
    else if (code == KEY_RIGHT) { if (cursor < text_len) cursor++; }
    else if (code == KEY_HOME) cursor = line_at[line];
    else if (code == KEY_END) cursor = line_at[line] + line_end_col(line);
    else if (code == KEY_UP && line > 0) {
        int maxc = line_end_col(line - 1);
        if (col > maxc) col = maxc;
        cursor = line_at[line - 1] + col;
    } else if (code == KEY_DOWN && line + 1 < nlines) {
        int maxc = line_end_col(line + 1);
        if (col > maxc) col = maxc;
        cursor = line_at[line + 1] + col;
    } else if (code >= 32 && code < 127) { insert_ch((char)code); dirty = 1; }
    if (cursor < 0) cursor = 0;
    if (cursor > text_len) cursor = text_len;
}

static void open_window(void) {
    ensure_buf();
    if (win) { gui_restore(win); return; }
    int ww = 520, wh = 380;
    if (ww > gfx_width() - 20) ww = gfx_width() - 20;
    if (wh > gfx_height() - 20) wh = gfx_height() - 20;
    win = gui_create("Notepad", ww, wh, ICON_NOTE, notepad_event, &win);
    if (!win) return;
    win->ask_close = notepad_ask;
    win->text_keys = 1;
    window_add_menu(win, "File");
    window_add_item(win, 0, "New", 1);
    window_add_item(win, 0, "Open...", 2);
    window_add_item(win, 0, "Save", 3);
    window_add_item(win, 0, "Save As...", 4);
    window_add_item(win, 0, "Exit", 5);
    set_title();
    serial_write("APP: NOTEPAD\n");
}

void app_notepad_open(void) {
    open_window();
}

void app_notepad_load(int inode) {
    ensure_buf();
    open_window();
    if (!win || inode <= 0 || fs_is_dir(inode)) return;
    uint32_t sz = 0;
    if (fs_read(inode, textb, NOTE_MAX - 1, &sz)) {
        gui_dialog("Notepad", "Could not read that file.", "OK", 0, 0);
        return;
    }
    text_len = (int)sz;
    textb[text_len] = 0;
    cursor = 0;
    scroll = 0;
    parent = fs_parent(inode);
    strncpy(filename, fs_name(inode), 28);
    dirty = 0;
    set_title();
    gui_focus(win);
}
