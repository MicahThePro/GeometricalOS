#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "input.h"
#include "serial.h"
#include "io.h"

#define CARD_MAX 4096

static Window *win;
static char title[28] = "Untitled";
static char body[CARD_MAX + 1];
static char filebuf[CARD_MAX + 64];
static int length, cursor, scroll, dirty;
static int line_at[512], line_count;
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
    while (i < length && line_count < 510) {
        line_at[line_count++] = i;
        int col = 0;
        while (i < length && body[i] != '\n' && col < cols) { i++; col++; }
        if (i < length && body[i] == '\n') i++;
    }
}

static int line_end(int line) {
    int end = line + 1 < line_count ? line_at[line + 1] : length;
    if (end > line_at[line] && body[end - 1] == '\n') end--;
    return end;
}

static void insert_char(char c) {
    if (length >= CARD_MAX - 1) return;
    memmove(body + cursor + 1, body + cursor, (size_t)(length - cursor + 1));
    body[cursor++] = c;
    length++;
    dirty = 1;
}

static void delete_char(int at) {
    if (at < 0 || at >= length) return;
    memmove(body + at, body + at + 1, (size_t)(length - at));
    length--;
    dirty = 1;
}

static int save_card(void) {
    if (!filename[0]) {
        char name[28] = "CARD.CRD";
        uint16_t dir = parent;
        if (!gui_file("Save Card", 1, &dir, name, sizeof(name))) return 0;
        strncpy(filename, name, sizeof(filename));
        parent = dir;
    }
    int n = 0;
    memcpy(filebuf, "GOSCRD1\n", 8);
    n = 8;
    strncpy(filebuf + n, title, 27);
    n += (int)strlen(title);
    filebuf[n++] = '\n';
    memcpy(filebuf + n, body, (size_t)length);
    n += length;
    if (fs_write(parent, filename, filebuf, (uint32_t)n) < 0) {
        gui_dialog("Cardfile", "Could not save this card.", "OK", 0, 0);
        return 0;
    }
    dirty = 0;
    return 1;
}

static int confirm_close(void) {
    if (!dirty) return 1;
    int r = gui_dialog("Cardfile", "Save changes to this card?", "Yes", "No", "Cancel");
    if (r == 1) return save_card();
    return r == 2;
}

static void load_card(int inode) {
    uint32_t size = 0;
    if (inode <= 0 || fs_is_dir(inode) ||
        fs_read(inode, filebuf, sizeof(filebuf) - 1, &size) ||
        size < 9 || memcmp(filebuf, "GOSCRD1\n", 8)) {
        gui_dialog("Cardfile", "That file is not a valid card.", "OK", 0, 0);
        return;
    }
    int end = 8;
    while ((uint32_t)end < size && filebuf[end] != '\n') end++;
    if ((uint32_t)end >= size || end - 8 > 27 || size - (uint32_t)end - 1 > CARD_MAX) {
        gui_dialog("Cardfile", "The card is damaged or too large.", "OK", 0, 0);
        return;
    }
    memcpy(title, filebuf + 8, (size_t)(end - 8));
    title[end - 8] = 0;
    length = (int)(size - (uint32_t)end - 1);
    memcpy(body, filebuf + end + 1, (size_t)length);
    body[length] = 0;
    cursor = scroll = 0;
    parent = fs_parent(inode);
    strncpy(filename, fs_name(inode), sizeof(filename));
    dirty = 0;
    app_recent_add(inode);
}

static void open_card(void) {
    if (!confirm_close()) return;
    char name[28] = "";
    uint16_t dir = parent;
    if (!gui_file("Open Card", 0, &dir, name, sizeof(name))) return;
    int id = fs_lookup(dir, name);
    if (id < 0) {
        gui_dialog("Cardfile", "That card is not in this folder.", "OK", 0, 0);
        return;
    }
    load_card(id);
}

static void new_card(void) {
    if (!confirm_close()) return;
    char name[28] = "Untitled";
    if (!gui_prompt("New Card", "Card title:", name, sizeof(name))) return;
    strncpy(title, name, sizeof(title));
    title[sizeof(title) - 1] = 0;
    body[0] = 0;
    filename[0] = 0;
    length = cursor = scroll = dirty = 0;
    parent = FS_ROOT;
}

static void card_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int text_y = 38;
    int cols = (cw - 20) / 8;
    int rows = (ch - text_y) / gfx_font_h();
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    reflow(cols);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 10, "Card title:", COL_BLACK, COL_GRAY);
        gfx_sunken(cx + 88, cy + 6, cw - 100, 24, COL_WHITE);
        gfx_text(cx + 94, cy + 10, title, COL_BLACK, COL_WHITE);
        gfx_sunken(cx + 8, cy + 34, cw - 16, ch - 42, COL_WHITE);
        int line = 0, col = 0;
        for (int i = 0; i < line_count; i++) {
            if (cursor >= line_at[i] &&
                cursor <= (i + 1 < line_count ? line_at[i + 1] : length)) {
                line = i;
                col = cursor - line_at[i];
                break;
            }
        }
        if (line_count && line < scroll) scroll = line;
        if (line_count && line >= scroll + rows) scroll = line - rows + 1;
        for (int r = 0; r < rows; r++) {
            int li = scroll + r;
            if (li >= line_count) break;
            int start = line_at[li], end = line_end(li);
            for (int i = start; i < end; i++)
                gfx_char(cx + 14 + (i - start) * 8, cy + text_y + r * gfx_font_h(),
                         (unsigned char)body[i], COL_BLACK, COL_WHITE, 1);
        }
        if ((timer_ticks() / 5) & 1 && line_count) {
            int li, ci = 0;
            for (li = 0; li + 1 < line_count && cursor >= line_at[li + 1]; li++) {}
            ci = cursor - line_at[li];
            gfx_fill(cx + 14 + ci * 8, cy + text_y + (li - scroll) * gfx_font_h(),
                     2, gfx_font_h(), COL_BLACK);
        }
        gfx_text(cx + 10, cy + ch - gfx_font_h() - 2,
                 "Type notes. File menu: New, Open, Save.", COL_DKGRAY, COL_GRAY);
        (void)col;
        return;
    }
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) new_card();
        else if (e->menu_id == 2) open_card();
        else if (e->menu_id == 3) save_card();
        else if (e->menu_id == 4 && confirm_close()) gui_close(w);
        return;
    }
    if (e->type == EVT_DOWN) {
        if (e->y >= text_y) {
            int li = scroll + (e->y - text_y) / gfx_font_h();
            if (li >= 0 && li < line_count) {
                int col = (e->x - 14) / 8;
                int max = line_end(li) - line_at[li];
                if (col < 0) col = 0;
                if (col > max) col = max;
                cursor = line_at[li] + col;
            }
        }
        return;
    }
    if (e->type != EVT_KEY) return;
    int line = 0, col = 0;
    for (line = 0; line + 1 < line_count && cursor >= line_at[line + 1]; line++) {}
    col = cursor - line_at[line];
    if (e->ctrl && (e->code == 's' || e->code == 'S')) save_card();
    else if (e->ctrl && (e->code == 'n' || e->code == 'N')) new_card();
    else if (e->ctrl && (e->code == 'o' || e->code == 'O')) open_card();
    else if (e->code == 8 && cursor > 0) delete_char(--cursor);
    else if (e->code == KEY_DEL) delete_char(cursor);
    else if (e->code == 10 || e->code == 13) insert_char('\n');
    else if (e->code == KEY_LEFT && cursor > 0) cursor--;
    else if (e->code == KEY_RIGHT && cursor < length) cursor++;
    else if (e->code == KEY_HOME) cursor = line_at[line];
    else if (e->code == KEY_END) cursor = line_end(line);
    else if (e->code == KEY_UP || e->code == KEY_DOWN) {
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
    int ww = 500, wh = 360;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("Cardfile", ww, wh, ICON_CARDFILE, card_event, &win);
    if (!win) return;
    win->ask_close = ask_close;
    win->text_keys = 1;
    window_add_menu(win, "File");
    window_add_item(win, 0, "New Card...", 1);
    window_add_item(win, 0, "Open...", 2);
    window_add_item(win, 0, "Save", 3);
    window_add_item(win, 0, "Exit", 4);
    serial_write("APP: CARDFILE\n");
}

void app_cardfile_open(void) {
    open_window();
}

void app_cardfile_load(int inode) {
    if (!confirm_close()) return;
    open_window();
    if (!win) return;
    load_card(inode);
    gui_focus(win);
}
