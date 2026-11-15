#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "io.h"
#include "serial.h"

#define WRITE_MAX 8192
#define WRI_HEADER 16

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t length;
    uint32_t alignment;
} WriHeader;

static Window *win;
static char text[WRITE_MAX + 1];
static uint8_t styles[WRITE_MAX];
static uint8_t filebuf[WRI_HEADER + WRITE_MAX * 2];
static int length, cursor, scroll, dirty, current_style, alignment;
static int line_at[1024], line_count;
static char filename[28];
static uint16_t parent = FS_ROOT;

static void reflow(int cols) {
    if (cols < 1) cols = 1;
    line_count = 0;
    int i = 0;
    if (!length) {
        line_at[line_count++] = 0;
        return;
    }
    while (i < length && line_count < 1022) {
        line_at[line_count++] = i;
        int col = 0;
        while (i < length && text[i] != '\n' && col < cols) { i++; col++; }
        if (i < length && text[i] == '\n') i++;
    }
    if (text[length - 1] == '\n' && line_count < 1023)
        line_at[line_count++] = length;
}

static int line_end(int line) {
    int end = line + 1 < line_count ? line_at[line + 1] : length;
    if (end > line_at[line] && text[end - 1] == '\n') end--;
    return end;
}

static void cursor_position(int cols, int *line, int *col) {
    reflow(cols);
    *line = 0;
    *col = 0;
    for (int i = 0; i < line_count; i++) {
        int start = line_at[i];
        int end = i + 1 < line_count ? line_at[i + 1] : length + 1;
        if (cursor >= start && cursor < end) {
            *line = i;
            *col = cursor - start;
            return;
        }
    }
    if (line_count) {
        *line = line_count - 1;
        *col = cursor - line_at[*line];
    }
}

static void set_title(void) {
    if (!win) return;
    if (filename[0]) {
        strncpy(win->title, filename, sizeof(win->title));
        strcpy(win->title + strlen(win->title), " - Write");
    } else {
        strncpy(win->title, "Untitled - Write", sizeof(win->title));
    }
}

static void insert_char(char c) {
    if (length >= WRITE_MAX - 1) return;
    memmove(text + cursor + 1, text + cursor, (size_t)(length - cursor + 1));
    memmove(styles + cursor + 1, styles + cursor, (size_t)(length - cursor));
    text[cursor] = c;
    styles[cursor] = (uint8_t)current_style;
    cursor++;
    length++;
    dirty = 1;
}

static void remove_char(int at) {
    if (at < 0 || at >= length) return;
    memmove(text + at, text + at + 1, (size_t)(length - at));
    memmove(styles + at, styles + at + 1, (size_t)(length - at - 1));
    length--;
    dirty = 1;
}

static int save_document(void) {
    if (!filename[0]) {
        char name[28] = "DOCUMENT.WRI";
        uint16_t dir = parent;
        if (!gui_file("Save Write document", 1, &dir, name, sizeof(name))) return 0;
        strncpy(filename, name, sizeof(filename));
        parent = dir;
    }
    WriHeader *head = (WriHeader *)filebuf;
    memcpy(head->magic, "GOSWRI1", 7);
    head->magic[7] = 0;
    head->length = (uint32_t)length;
    head->alignment = (uint32_t)alignment;
    memcpy(filebuf + WRI_HEADER, text, (size_t)length);
    memcpy(filebuf + WRI_HEADER + length, styles, (size_t)length);
    if (fs_write(parent, filename, filebuf,
                 (uint32_t)(WRI_HEADER + length * 2)) < 0) {
        gui_dialog("Write", "Could not save this document.", "OK", 0, 0);
        return 0;
    }
    dirty = 0;
    set_title();
    return 1;
}

static int confirm_close(void) {
    if (!dirty) return 1;
    int result = gui_dialog("Write", "Save changes to this document?",
                            "Yes", "No", "Cancel");
    if (result == 1) return save_document();
    return result == 2;
}

static void load_inode(int inode) {
    if (inode <= 0 || fs_is_dir(inode)) return;
    uint32_t size = 0;
    if (fs_read(inode, filebuf, sizeof(filebuf), &size) ||
        size < WRI_HEADER || memcmp(filebuf, "GOSWRI1", 7)) {
        gui_dialog("Write", "That file is not a valid Write document.", "OK", 0, 0);
        return;
    }
    WriHeader *head = (WriHeader *)filebuf;
    if (head->length > WRITE_MAX || size < WRI_HEADER + head->length * 2u) {
        gui_dialog("Write", "The Write document is damaged or too large.", "OK", 0, 0);
        return;
    }
    length = (int)head->length;
    alignment = (int)(head->alignment > 2 ? 0 : head->alignment);
    memcpy(text, filebuf + WRI_HEADER, (size_t)length);
    memcpy(styles, filebuf + WRI_HEADER + length, (size_t)length);
    text[length] = 0;
    cursor = scroll = 0;
    current_style = 0;
    parent = fs_parent(inode);
    strncpy(filename, fs_name(inode), sizeof(filename));
    dirty = 0;
    set_title();
}

static void open_file(void) {
    if (!confirm_close()) return;
    char name[28] = "";
    uint16_t dir = parent;
    if (!gui_file("Open Write document", 0, &dir, name, sizeof(name))) return;
    int id = fs_lookup(dir, name);
    if (id < 0) {
        gui_dialog("Write", "That file is not in this folder.", "OK", 0, 0);
        return;
    }
    load_inode(id);
}

static void new_document(void) {
    if (!confirm_close()) return;
    length = cursor = scroll = dirty = current_style = alignment = 0;
    text[0] = 0;
    filename[0] = 0;
    parent = FS_ROOT;
    set_title();
}

static void draw_line(int x, int y, int start, int end) {
    for (int i = start; i < end; i++) {
        uint8_t s = styles[i];
        gfx_char_style(x, y, (unsigned char)text[i], COL_BLACK, COL_WHITE, 1,
                       (s & 1) != 0, (s & 2) != 0, (s & 4) != 0);
        x += 8;
    }
}

static void write_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int text_y = 36;
    int cols = (cw - 12) / 8;
    int rows = (ch - text_y) / gfx_font_h();
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy, cw, 32, COL_GRAY);
        gfx_button(cx + 6, cy + 4, 38, 24, "B", (current_style & 1) != 0);
        gfx_button(cx + 48, cy + 4, 38, 24, "I", (current_style & 2) != 0);
        gfx_button(cx + 90, cy + 4, 38, 24, "U", (current_style & 4) != 0);
        gfx_button(cx + 134, cy + 4, 78, 24,
                   alignment == 0 ? "Left" : (alignment == 1 ? "Center" : "Right"), 0);
        int line, col;
        cursor_position(cols, &line, &col);
        if (line < scroll) scroll = line;
        if (line >= scroll + rows) scroll = line - rows + 1;
        for (int r = 0; r < rows; r++) {
            int li = scroll + r;
            if (li >= line_count) break;
            int start = line_at[li], end = line_end(li);
            int width = (end - start) * 8;
            int x = cx + 6;
            if (alignment == 1) x += (cw - width) / 2;
            else if (alignment == 2) x += cw - width - 6;
            draw_line(x, cy + text_y + r * gfx_font_h(), start, end);
        }
        if ((timer_ticks() / 5) & 1) {
            int li, col0;
            cursor_position(cols, &li, &col0);
            int start = line_at[li];
            int width = col0 * 8;
            int x = cx + 6 + width;
            int line_width = (line_end(li) - start) * 8;
            if (alignment == 1) x += (cw - line_width) / 2;
            else if (alignment == 2) x += cw - line_width - 6;
            gfx_fill(x, cy + text_y + (li - scroll) * gfx_font_h(),
                     2, gfx_font_h(), COL_BLACK);
        }
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) new_document();
        else if (e->menu_id == 2) open_file();
        else if (e->menu_id == 3) save_document();
        else if (e->menu_id == 4) {
            if (confirm_close()) gui_close(w);
        }
        else if (e->menu_id == 5) current_style ^= 1;
        else if (e->menu_id == 6) current_style ^= 2;
        else if (e->menu_id == 7) current_style ^= 4;
        else if (e->menu_id == 8) alignment = (alignment + 1) % 3;
        return;
    }
    if (e->type == EVT_DOWN) {
        if (e->y < 32) {
            if (e->x >= 6 && e->x < 44) current_style ^= 1;
            else if (e->x >= 48 && e->x < 86) current_style ^= 2;
            else if (e->x >= 90 && e->x < 128) current_style ^= 4;
            else if (e->x >= 134 && e->x < 212) alignment = (alignment + 1) % 3;
            return;
        }
        int li = scroll + (e->y - text_y) / gfx_font_h();
        if (li < 0) li = 0;
        if (li >= line_count) li = line_count - 1;
        int col = (e->x - 6) / 8;
        if (col < 0) col = 0;
        int max = line_end(li) - line_at[li];
        if (col > max) col = max;
        cursor = line_at[li] + col;
        return;
    }
    if (e->type != EVT_KEY) return;
    int line, col;
    cursor_position(cols, &line, &col);
    if (e->ctrl && (e->code == 'b' || e->code == 'B')) current_style ^= 1;
    else if (e->ctrl && (e->code == 'i' || e->code == 'I')) current_style ^= 2;
    else if (e->ctrl && (e->code == 'u' || e->code == 'U')) current_style ^= 4;
    else if (e->ctrl && (e->code == 's' || e->code == 'S')) save_document();
    else if (e->ctrl && (e->code == 'v' || e->code == 'V'))
        insert_char((char)app_charmap_clipboard());
    else if (e->code == 8) { if (cursor > 0) { remove_char(--cursor); } }
    else if (e->code == KEY_DEL) remove_char(cursor);
    else if (e->code == 10 || e->code == 13) insert_char('\n');
    else if (e->code == KEY_LEFT && cursor > 0) cursor--;
    else if (e->code == KEY_RIGHT && cursor < length) cursor++;
    else if (e->code == KEY_HOME) {
        cursor_position(cols, &line, &col);
        cursor = line_at[line];
    } else if (e->code == KEY_END) {
        cursor_position(cols, &line, &col);
        cursor = line_end(line);
    } else if (e->code == KEY_UP || e->code == KEY_DOWN) {
        cursor_position(cols, &line, &col);
        int next = line + (e->code == KEY_UP ? -1 : 1);
        if (next >= 0 && next < line_count) {
            int n = line_end(next) - line_at[next];
            cursor = line_at[next] + (col < n ? col : n);
        }
    } else if (e->code >= 32 && e->code < 127) insert_char((char)e->code);
}

static int ask_close(Window *w) {
    (void)w;
    return confirm_close();
}

static void open_window(void) {
    if (win) { gui_restore(win); return; }
    int ww = 560, wh = 400;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("Untitled - Write", ww, wh, ICON_WRITE, write_event, &win);
    if (!win) return;
    win->ask_close = ask_close;
    win->text_keys = 1;
    window_add_menu(win, "File");
    window_add_item(win, 0, "New", 1);
    window_add_item(win, 0, "Open...", 2);
    window_add_item(win, 0, "Save", 3);
    window_add_item(win, 0, "Exit", 4);
    window_add_menu(win, "Format");
    window_add_item(win, 1, "Toggle Bold (Ctrl+B)", 5);
    window_add_item(win, 1, "Toggle Italic (Ctrl+I)", 6);
    window_add_item(win, 1, "Toggle Underline (Ctrl+U)", 7);
    window_add_item(win, 1, "Change Alignment", 8);
    serial_write("APP: WRITE\n");
}

void app_write_open(void) {
    open_window();
}

void app_write_load(int inode) {
    if (!confirm_close()) return;
    open_window();
    if (!win) return;
    load_inode(inode);
    gui_focus(win);
}
