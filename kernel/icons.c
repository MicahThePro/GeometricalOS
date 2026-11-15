#include "gui.h"
#include "gfx.h"

static void frame(int x, int y) {
    gfx_raised(x, y, 32, 32, COL_GRAY);
}

void icons_draw(int id, int x, int y) {
    if (id == ICON_GROUP) {
        frame(x, y);
        gfx_fill(x + 3, y + 3, 26, 8, COL_NAVY);
        gfx_fill(x + 6, y + 16, 20, 10, 0xE0C040);
        gfx_rect(x + 6, y + 16, 20, 10, COL_BLACK);
        return;
    }
    if (id == ICON_FILES) {
        gfx_fill(x + 2, y + 8, 14, 8, 0xE0C040);
        gfx_fill(x + 2, y + 12, 28, 16, 0xF0D060);
        gfx_rect(x + 2, y + 12, 28, 16, COL_BLACK);
        gfx_rect(x + 2, y + 8, 14, 8, COL_BLACK);
        return;
    }
    if (id == ICON_NOTE) {
        gfx_fill(x + 6, y + 2, 18, 26, COL_WHITE);
        gfx_rect(x + 6, y + 2, 18, 26, COL_BLACK);
        gfx_fill(x + 18, y + 2, 6, 6, COL_GRAY);
        for (int i = 0; i < 4; i++) gfx_hline(x + 9, y + 10 + i * 4, 12, 0x5080C0);
        gfx_line(x + 22, y + 22, x + 30, y + 30, 0xC06020);
        return;
    }
    if (id == ICON_CALC) {
        gfx_raised(x + 4, y + 2, 24, 28, COL_GRAY);
        gfx_sunken(x + 7, y + 5, 18, 7, COL_NAVY);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                gfx_raised(x + 7 + c * 6, y + 15 + r * 5, 5, 4, COL_WHITE);
        return;
    }
    if (id == ICON_CLOCK) {
        gfx_fill_circle(x + 16, y + 16, 13, COL_WHITE);
        gfx_circle(x + 16, y + 16, 13, COL_BLACK);
        gfx_line(x + 16, y + 16, x + 16, y + 7, COL_BLACK);
        gfx_line(x + 16, y + 16, x + 23, y + 18, COL_NAVY);
        return;
    }
    if (id == ICON_PAINT) {
        gfx_fill_circle(x + 12, y + 14, 10, 0xF0F0F0);
        gfx_circle(x + 12, y + 14, 10, COL_BLACK);
        gfx_fill_circle(x + 8, y + 12, 3, COL_RED);
        gfx_fill_circle(x + 14, y + 10, 3, COL_YELLOW);
        gfx_fill_circle(x + 15, y + 16, 3, COL_LTBLUE);
        gfx_fill_circle(x + 9, y + 17, 3, COL_GREEN);
        gfx_line(x + 20, y + 20, x + 30, y + 6, 0x404040);
        return;
    }
    if (id == ICON_TERM) {
        gfx_fill(x + 2, y + 6, 28, 20, COL_BLACK);
        gfx_rect(x + 2, y + 6, 28, 20, COL_WHITE);
        gfx_text(x + 6, y + 10, ">", COL_LTGREEN, COL_BLACK);
        return;
    }
    if (id == ICON_SOL) {
        gfx_raised(x + 6, y + 2, 20, 28, COL_WHITE);
        gfx_text(x + 8, y + 6, "A", COL_RED, COL_WHITE);
        gfx_fill_triangle(x + 12, y + 18, x + 16, y + 26, x + 20, y + 18, COL_RED);
        return;
    }
    if (id == ICON_WEB) {
        gfx_fill_circle(x + 16, y + 16, 12, COL_LTBLUE);
        gfx_circle(x + 16, y + 16, 12, COL_WHITE);
        gfx_hline(x + 4, y + 16, 24, COL_WHITE);
        gfx_vline(x + 16, y + 4, 24, COL_WHITE);
        return;
    }
    if (id == ICON_MINE) {
        gfx_fill_circle(x + 16, y + 16, 7, COL_BLACK);
        gfx_line(x + 16, y + 3, x + 16, y + 29, COL_BLACK);
        gfx_line(x + 3, y + 16, x + 29, y + 16, COL_BLACK);
        gfx_line(x + 6, y + 6, x + 26, y + 26, COL_BLACK);
        gfx_line(x + 26, y + 6, x + 6, y + 26, COL_BLACK);
        gfx_fill_circle(x + 13, y + 13, 2, COL_WHITE);
        return;
    }
    frame(x, y);
}
