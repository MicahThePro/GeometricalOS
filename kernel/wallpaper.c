#include "wallpaper.h"
#include "gui.h"
#include "gfx.h"
#include "io.h"
#include "settings.h"

static int wall_id;

void wall_load(void) {
    wall_id = settings_wallpaper();
}

void wall_set(int id) {
    if (id < 0) id = 0;
    if (id > 4) id = 4;
    wall_id = id;
    settings_set_wallpaper(id);
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
    } else if (wall_id == 4) {
        for (int y = 0; y < h; y += 64) {
            for (int x = 0; x < w; x += 64) {
                gfx_line(x + 32, y + 4, x + 60, y + 32, COL_GEO_MID);
                gfx_line(x + 60, y + 32, x + 32, y + 60, COL_GEO_MID);
                gfx_line(x + 32, y + 60, x + 4, y + 32, COL_GEO_MID);
                gfx_line(x + 4, y + 32, x + 32, y + 4, COL_GEO_MID);
                if (((x / 64) + (y / 64)) % 3 == 0)
                    gfx_fill_triangle(x + 28, y + 32, x + 32, y + 24,
                                      x + 36, y + 32, 0x98722Fu);
            }
        }
    }
}

static const char *NAMES[] = {
    "Solid",
    "Tiles",
    "Stripes",
    "Diamonds",
    "Orbit Grid"
};

static void wall_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, "Choose a desktop pattern", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 5; i++) {
            int y = cy + 36 + i * 32;
            gfx_button(cx + 16, y, cw - 32, 26, NAMES[i], wall_id == i);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < 5; i++) {
            int y = 36 + i * 32;
            if (e->x >= 16 && e->x < cw - 16 && e->y >= y && e->y < y + 26) {
                wall_set(i);
                gui_invalidate();
                return;
            }
        }
    }
}

void app_wall_open(void) {
    static Window *win;
    if (win) { gui_restore(win); return; }
    win = gui_create("Wallpaper", 280, 230, ICON_WALLPAPER, wall_event, &win);
}
