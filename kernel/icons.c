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
    if (id == ICON_SNAKE) {
        gfx_fill(x + 3, y + 3, 26, 26, 0x004020);
        gfx_rect(x + 3, y + 3, 26, 26, COL_BLACK);
        gfx_fill(x + 7, y + 18, 6, 6, 0x00C040);
        gfx_fill(x + 13, y + 18, 6, 6, 0x00C040);
        gfx_fill(x + 13, y + 12, 6, 6, 0x00C040);
        gfx_fill(x + 19, y + 12, 6, 6, 0x80FF80);
        gfx_fill_circle(x + 25, y + 6, 3, COL_RED);
        return;
    }
    if (id == ICON_WRITE) {
        gfx_fill(x + 5, y + 3, 22, 26, COL_WHITE);
        gfx_rect(x + 5, y + 3, 22, 26, COL_BLACK);
        gfx_text(x + 8, y + 7, "W", COL_NAVY, COL_WHITE);
        gfx_hline(x + 8, y + 24, 15, COL_DKGRAY);
        return;
    }
    if (id == ICON_CHARMAP) {
        gfx_raised(x + 3, y + 3, 26, 26, COL_GRAY);
        gfx_text(x + 8, y + 7, "A", COL_BLACK, COL_GRAY);
        gfx_text(x + 17, y + 17, "?", COL_NAVY, COL_GRAY);
        return;
    }
    if (id == ICON_CARDFILE) {
        gfx_fill(x + 5, y + 5, 23, 24, COL_BLUE);
        gfx_rect(x + 5, y + 5, 23, 24, COL_BLACK);
        gfx_fill(x + 2, y + 2, 20, 24, COL_YELLOW);
        gfx_rect(x + 2, y + 2, 20, 24, COL_BLACK);
        gfx_hline(x + 6, y + 9, 12, COL_NAVY);
        gfx_hline(x + 6, y + 14, 12, COL_NAVY);
        return;
    }
    if (id == ICON_RECYCLE) {
        gfx_raised(x + 5, y + 3, 22, 26, COL_WHITE);
        gfx_fill(x + 9, y + 9, 14, 15, 0xD0E8F0);
        gfx_rect(x + 9, y + 9, 14, 15, COL_BLACK);
        gfx_hline(x + 8, y + 7, 16, COL_BLACK);
        gfx_line(x + 13, y + 12, x + 19, y + 20, COL_GREEN);
        gfx_line(x + 19, y + 12, x + 13, y + 20, COL_GREEN);
        return;
    }
    if (id == ICON_SYSTEM) {
        gfx_fill(x + 3, y + 4, 26, 19, COL_BLACK);
        gfx_rect(x + 3, y + 4, 26, 19, COL_DKGRAY);
        gfx_fill(x + 6, y + 7, 20, 13, 0xC0D8D8);
        gfx_hline(x + 13, y + 24, 7, COL_BLACK);
        gfx_vline(x + 16, y + 22, 4, COL_BLACK);
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
