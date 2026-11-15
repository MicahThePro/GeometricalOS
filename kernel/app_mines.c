#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "rtc.h"
#include "input.h"
#include "io.h"
#include "serial.h"

#define COLS 9
#define ROWS 9
#define MINES 10
#define CELL 22

static Window *win;
static uint8_t mine[ROWS][COLS];
static uint8_t cover[ROWS][COLS]; /* 0 hidden, 1 open, 2 flag */
static uint8_t nearv[ROWS][COLS];
static int placed, dead, won, opened, flags;
static int start_sec, elapsed, running;
static uint32_t rng = 0x12345678u;

static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static void recount(void) {
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) continue;
                    n += mine[ny][nx];
                }
            nearv[y][x] = (uint8_t)n;
        }
    }
}

static void reset_game(void) {
    memset(mine, 0, sizeof(mine));
    memset(cover, 0, sizeof(cover));
    memset(nearv, 0, sizeof(nearv));
    placed = dead = won = opened = flags = running = elapsed = 0;
    rng ^= (uint32_t)rtc_seconds_of_day() * 2654435761u;
    rng ^= timer_ticks() | 1u;
}

static void place(int sx, int sy) {
    int left = MINES;
    int guard = 0;
    while (left && guard++ < 5000) {
        int x = (int)(rnd() % COLS);
        int y = (int)(rnd() % ROWS);
        if (mine[y][x]) continue;
        if (x == sx && y == sy) continue;
        mine[y][x] = 1;
        left--;
    }
    recount();
    placed = 1;
    running = 1;
    start_sec = rtc_seconds_of_day();
}

static void open_cell(int x, int y) {
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return;
    if (cover[y][x] == 1 || cover[y][x] == 2) return;
    cover[y][x] = 1;
    opened++;
    if (mine[y][x]) { dead = 1; running = 0; return; }
    if (nearv[y][x] == 0) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) open_cell(x + dx, y + dy);
    }
}

static uint32_t num_color(int n) {
    static const uint32_t c[] = {
        0, 0x0000FF, 0x008000, 0xFF0000, 0x000080, 0x800000, 0x008080, 0x000000, 0x808080
    };
    if (n < 0 || n > 8) return COL_BLACK;
    return c[n];
}

static void draw_digit(int x, int y, int v, uint32_t fg) {
    char s[2] = {(char)('0' + (v % 10)), 0};
    gfx_char(x, y, (unsigned char)s[0], fg, COL_BLACK, 1);
}

static void mines_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int gx = cx + (cw - COLS * CELL) / 2;
    int gy = cy + 48;
    if (running && !dead && !won) {
        int d = rtc_seconds_of_day() - start_sec;
        if (d < 0) d += 86400;
        if (d > 999) d = 999;
        elapsed = d;
    }
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_sunken(cx + 8, cy + 8, cw - 16, 32, COL_GRAY);
        int mines_left = MINES - flags;
        if (mines_left < 0) mines_left = 0;
        gfx_fill(cx + 14, cy + 14, 40, 20, COL_BLACK);
        draw_digit(cx + 16, cy + 16, mines_left / 100, COL_RED);
        draw_digit(cx + 28, cy + 16, (mines_left / 10) % 10, COL_RED);
        draw_digit(cx + 40, cy + 16, mines_left % 10, COL_RED);
        gfx_button(cx + cw / 2 - 14, cy + 12, 28, 24, dead ? ":(" : (won ? "B)" : ":)"), 0);
        gfx_fill(cx + cw - 56, cy + 14, 40, 20, COL_BLACK);
        draw_digit(cx + cw - 54, cy + 16, elapsed / 100, COL_RED);
        draw_digit(cx + cw - 42, cy + 16, (elapsed / 10) % 10, COL_RED);
        draw_digit(cx + cw - 30, cy + 16, elapsed % 10, COL_RED);
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                int px = gx + x * CELL, py = gy + y * CELL;
                int open = cover[y][x] == 1 || (dead && mine[y][x]);
                if (!open) {
                    gfx_raised(px, py, CELL - 1, CELL - 1, COL_GRAY);
                    if (cover[y][x] == 2) {
                        gfx_vline(px + 14, py + 4, 12, COL_BLACK);
                        gfx_fill_triangle(px + 6, py + 5, px + 14, py + 8, px + 6, py + 11, COL_RED);
                    }
                } else {
                    gfx_fill(px, py, CELL - 1, CELL - 1, COL_GRAY);
                    gfx_hline(px, py, CELL - 1, COL_DKGRAY);
                    gfx_vline(px, py, CELL - 1, COL_DKGRAY);
                    if (mine[y][x]) {
                        gfx_fill_circle(px + 10, py + 10, 6, COL_BLACK);
                        if (cover[y][x] == 1 && dead)
                            gfx_fill(px, py, CELL - 1, CELL - 1, COL_RED),
                            gfx_fill_circle(px + 10, py + 10, 6, COL_BLACK);
                    } else if (nearv[y][x]) {
                        char s[2] = {(char)('0' + nearv[y][x]), 0};
                        gfx_text(px + 6, py + 3, s, num_color(nearv[y][x]), COL_GRAY);
                    }
                }
            }
        }
        if (dead) gfx_text(cx + 8, gy + ROWS * CELL + 6, "Boom! Click the face to try again.", COL_MAROON, COL_GRAY);
        else if (won) gfx_text(cx + 8, gy + ROWS * CELL + 6, "You win! Click the face to play again.", COL_NAVY, COL_GRAY);
        else gfx_text(cx + 8, gy + ROWS * CELL + 6, "Left click opens. Right click flags.", COL_BLACK, COL_GRAY);
        return;
    }
    if (e->type == EVT_DOWN && e->x >= cw / 2 - 14 && e->x < cw / 2 + 14 && e->y >= 12 && e->y < 36) {
        reset_game();
        return;
    }
    int lx = e->x - (gx - cx);
    int ly = e->y - (gy - cy);
    if (lx < 0 || ly < 0) return;
    int x = lx / CELL, y = ly / CELL;
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return;
    if (e->type == EVT_RDOWN) {
        if (dead || won) return;
        if (cover[y][x] == 0) { cover[y][x] = 2; flags++; }
        else if (cover[y][x] == 2) { cover[y][x] = 0; flags--; }
        return;
    }
    if (e->type != EVT_DOWN || dead || won) return;
    if (!placed) place(x, y);
    if (cover[y][x] == 2) return;
    open_cell(x, y);
    if (!dead && opened >= COLS * ROWS - MINES) { won = 1; running = 0; }
}

void app_mines_open(void) {
    if (!win) reset_game();
    if (win) { gui_restore(win); return; }
    win = gui_create("Minesweeper", 280, 320, ICON_MINE, mines_event, &win);
    serial_write("APP: MINES\n");
}
