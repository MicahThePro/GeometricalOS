#include "wallpaper.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "io.h"
#include "settings.h"

static int wall_id;

static void wall_store(void) {
    char b[4] = {(char)('0' + wall_id), '\n', 0, 0};
    fs_write(FS_ROOT, "WALL.CFG", b, 2);
}

void wall_load(void) {
    wall_id = 0;
    int id = fs_lookup(FS_ROOT, "WALL.CFG");
    if (id < 0) return;
    char b[8];
    uint32_t n = 0;
    if (fs_read(id, b, 7, &n) == 0 && b[0] >= '0' && b[0] <= '3')
        wall_id = b[0] - '0';
}

void wall_set(int id) {
    if (id < 0) id = 0;
    if (id > 3) id = 3;
    wall_id = id;
    wall_store();
}

int wall_get(void) { return wall_id; }

void wall_draw(void) {
    int w = gfx_width(), h = gfx_height();
    gfx_clear(settings_desktop_color());
    if (wall_id == 1) {
        for (int y = 0; y < h; y += 48)
            for (int x = 0; x < w; x += 48) {
                gfx_rect(x + 8, y + 8, 20, 20, 0x00A0A0);
                gfx_fill_circle(x + 34, y + 18, 6, 0x006868);
            }
    } else if (wall_id == 2) {
        for (int i = -h; i < w; i += 28)
            gfx_line(i, 0, i + h, h, 0x007070);
    } else if (wall_id == 3) {
        for (int y = 16; y < h; y += 36)
            for (int x = 16; x < w; x += 36) {
                gfx_fill_triangle(x, y + 10, x + 10, y, x + 20, y + 10, 0x20A0A0);
                gfx_fill_triangle(x, y + 10, x + 10, y + 20, x + 20, y + 10, 0x006060);
            }
    }
}

static const char *NAMES[] = {
    "Solid teal",
    "Tiles",
    "Stripes",
    "Diamonds"
};

static void wall_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, "Choose a desktop pattern", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 4; i++) {
            int y = cy + 36 + i * 36;
            gfx_button(cx + 16, y, cw - 32, 30, NAMES[i], wall_id == i);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < 4; i++) {
            int y = 36 + i * 36;
            if (e->x >= 16 && e->x < cw - 16 && e->y >= y && e->y < y + 30) {
                wall_id = i;
                wall_store();
                gui_invalidate();
                return;
            }
        }
    }
}

void app_wall_open(void) {
    static Window *win;
    if (win) { gui_restore(win); return; }
    win = gui_create("Wallpaper", 280, 230, ICON_GROUP, wall_event, &win);
}
