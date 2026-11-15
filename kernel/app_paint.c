#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "serial.h"

#define CW 480
#define CH 280

static Window *win;
static uint8_t *canvas;
static int tool; /* 0 pencil 1 line 2 rect 3 fill */
static int color = 0;
static int dragging, x0, y0, x1, y1;

static const uint32_t PAL[16] = {
    0x000000, 0x808080, 0x800000, 0xFF0000,
    0x008000, 0x00FF00, 0x808000, 0xFFFF00,
    0x000080, 0x0000FF, 0x008080, 0x00FFFF,
    0x800080, 0xFF00FF, 0xC0C0C0, 0xFFFFFF
};

static void ensure(void) {
    if (canvas) return;
    canvas = (uint8_t *)0x680000;
    for (int i = 0; i < CW * CH; i++) canvas[i] = 15;
}

static int ox(void) { return 64; }
static int oy(void) { return 8; }

static void cpixel(int x, int y, int col) {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return;
    canvas[y * CW + x] = (uint8_t)col;
}

static void cline(int xa, int ya, int xb, int yb, int col) {
    int dx = xb - xa, dy = yb - ya;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        cpixel(xa, ya, col);
        if (xa == xb && ya == yb) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; xa += sx; }
        if (e2 < dx) { err += dx; ya += sy; }
    }
}

static void crect(int xa, int ya, int xb, int yb, int col) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    if (ya > yb) { int t = ya; ya = yb; yb = t; }
    cline(xa, ya, xb, ya, col);
    cline(xa, yb, xb, yb, col);
    cline(xa, ya, xa, yb, col);
    cline(xb, ya, xb, yb, col);
}

static void flood(int x, int y, int repl) {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return;
    uint8_t target = canvas[y * CW + x];
    if (target == (uint8_t)repl) return;
    int *stack = (int *)0x6C0000;
    int sp = 0;
    stack[sp++] = x;
    stack[sp++] = y;
    while (sp >= 2) {
        int cy = stack[--sp];
        int cx = stack[--sp];
        if (cx < 0 || cy < 0 || cx >= CW || cy >= CH) continue;
        if (canvas[cy * CW + cx] != target) continue;
        int l = cx;
        while (l > 0 && canvas[cy * CW + (l - 1)] == target) l--;
        int r = cx;
        while (r + 1 < CW && canvas[cy * CW + (r + 1)] == target) r++;
        for (int i = l; i <= r; i++) canvas[cy * CW + i] = (uint8_t)repl;
        for (int i = l; i <= r; i++) {
            if (cy > 0 && canvas[(cy - 1) * CW + i] == target) {
                int edge = (i == l) || canvas[(cy - 1) * CW + (i - 1)] != target;
                if (edge && sp < 20000) { stack[sp++] = i; stack[sp++] = cy - 1; }
            }
            if (cy + 1 < CH && canvas[(cy + 1) * CW + i] == target) {
                int edge = (i == l) || canvas[(cy + 1) * CW + (i - 1)] != target;
                if (edge && sp < 20000) { stack[sp++] = i; stack[sp++] = cy + 1; }
            }
        }
    }
}

static int in_canvas(int x, int y) {
    return x >= ox() && y >= oy() && x < ox() + CW && y < oy() + CH;
}

static void paint_event(Window *w, Event *e) {
    ensure();
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        static const char *tools[] = {"Pen", "Line", "Box", "Fill"};
        for (int i = 0; i < 4; i++) {
            gfx_button(cx + 6, cy + 8 + i * 32, 52, 26, tools[i], tool == i);
        }
        gfx_button(cx + 6, cy + 8 + 4 * 32 + 8, 52, 26, "Clear", 0);
        gfx_blit_indexed(cx + ox(), cy + oy(), canvas, CW, CH, PAL);
        gfx_sunken(cx + ox() - 2, cy + oy() - 2, CW + 4, CH + 4, COL_WHITE);
        /* sunken draws over the edge; blit again inside */
        gfx_set_clip(cx + ox(), cy + oy(), CW, CH);
        gfx_blit_indexed(cx + ox(), cy + oy(), canvas, CW, CH, PAL);
        if (dragging && (tool == 1 || tool == 2)) {
            if (tool == 1) gfx_line(cx + ox() + x0, cy + oy() + y0, cx + ox() + x1, cy + oy() + y1, PAL[color]);
            else {
                int xa = x0, ya = y0, xb = x1, yb = y1;
                if (xa > xb) { int t = xa; xa = xb; xb = t; }
                if (ya > yb) { int t = ya; ya = yb; yb = t; }
                gfx_rect(cx + ox() + xa, cy + oy() + ya, xb - xa + 1, yb - ya + 1, PAL[color]);
            }
        }
        gfx_reset_clip();
        int py = cy + ch - 28;
        gfx_text(cx + 8, py + 4, "Color", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 16; i++) {
            int sx = cx + 64 + i * 22;
            gfx_fill(sx, py, 18, 18, PAL[i]);
            gfx_rect(sx, py, 18, 18, i == color ? COL_WHITE : COL_BLACK);
            if (i == color) gfx_rect(sx - 1, py - 1, 20, 20, COL_NAVY);
        }
        return;
    }
    if (e->type == EVT_KEY) {
        if (e->code == 'p' || e->code == 'P') tool = 0;
        else if (e->code == 'l' || e->code == 'L') tool = 1;
        else if (e->code == 'b' || e->code == 'B') tool = 2;
        else if (e->code == 'f' || e->code == 'F') tool = 3;
        else if (e->code == 'c' || e->code == 'C') {
            for (int i = 0; i < CW * CH; i++) canvas[i] = 15;
        }
        return;
    }
    int py = ch - 28;
    if (e->type == EVT_DOWN) {
        if (e->x >= 6 && e->x < 58) {
            int t = (e->y - 8) / 32;
            if (e->y >= 8 && t >= 0 && t < 4 && e->y < 8 + t * 32 + 26) tool = t;
            if (e->y >= 8 + 4 * 32 + 8 && e->y < 8 + 4 * 32 + 34) {
                for (int i = 0; i < CW * CH; i++) canvas[i] = 15;
            }
        }
        if (e->y >= py && e->y < py + 18) {
            int idx = (e->x - 64) / 22;
            if (idx >= 0 && idx < 16 && e->x >= 64) color = idx;
        }
        if (in_canvas(e->x, e->y)) {
            int lx = e->x - ox(), ly = e->y - oy();
            if (tool == 0) { cpixel(lx, ly, color); dragging = 1; x0 = lx; y0 = ly; }
            else if (tool == 1 || tool == 2) { dragging = 1; x0 = x1 = lx; y0 = y1 = ly; }
            else if (tool == 3) flood(lx, ly, color);
        }
        return;
    }
    if (e->type == EVT_MOVE && dragging && in_canvas(e->x, e->y)) {
        int lx = e->x - ox(), ly = e->y - oy();
        if (tool == 0) { cline(x0, y0, lx, ly, color); x0 = lx; y0 = ly; }
        else { x1 = lx; y1 = ly; }
        return;
    }
    if (e->type == EVT_UP && dragging) {
        if (tool == 1) cline(x0, y0, x1, y1, color);
        if (tool == 2) crect(x0, y0, x1, y1, color);
        dragging = 0;
    }
}

void app_paint_open(void) {
    ensure();
    if (win) { gui_restore(win); return; }
    int ww = 640, wh = 460;
    if (ww > gfx_width() - 10) ww = gfx_width() - 10;
    if (wh > gfx_height() - 10) wh = gfx_height() - 10;
    win = gui_create("Paint", ww, wh, ICON_PAINT, paint_event, &win);
    serial_write("APP: PAINT\n");
}
