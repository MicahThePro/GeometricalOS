#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "serial.h"
#include "io.h"

#define CW 480
#define CH 280

static Window *win;
static uint8_t *canvas;
static int tool; /* 0 pencil 1 line 2 rect 3 fill */
static int color = 0;
static int dragging, x0, y0, x1, y1;
static int dirty;
static char filename[28];
static uint16_t parent;
static uint8_t *filebuf;

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
    dirty = 1;
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

static void set_title(void) {
    char t[48];
    if (filename[0]) strcpy(t, filename);
    else strcpy(t, "Untitled");
    strncpy(t + strlen(t), " - Paint", 48 - strlen(t));
    if (win) strncpy(win->title, t, sizeof(win->title));
}

static int save_image(void) {
    ensure();
    if (!filebuf) filebuf = (uint8_t *)0x700000;
    if (!filename[0]) {
        char name[28] = "PICTURE.GIM";
        uint16_t dir = parent;
        if (!gui_file("Save As", 1, &dir, name, 28)) return 0;
        strncpy(filename, name, 28);
        parent = dir;
    }
    memcpy(filebuf, "GIMG", 4);
    filebuf[4] = (uint8_t)CW;
    filebuf[5] = (uint8_t)(CW >> 8);
    filebuf[6] = (uint8_t)CH;
    filebuf[7] = (uint8_t)(CH >> 8);
    memcpy(filebuf + 8, canvas, CW * CH);
    if (fs_write(parent, filename, filebuf, 8u + CW * CH) < 0) {
        gui_dialog("Paint", "Could not save the picture.", "OK", 0, 0);
        return 0;
    }
    dirty = 0;
    set_title();
    serial_write("PAINT: saved ");
    serial_write(filename);
    serial_write("\n");
    return 1;
}

static int confirm_close(void) {
    if (!dirty) return 1;
    int r = gui_dialog("Paint", "Save changes to this picture?", "Yes", "No", "Cancel");
    if (r == 1) return save_image();
    if (r == 2) return 1;
    return 0;
}

static int paint_ask(Window *w) {
    (void)w;
    return confirm_close();
}

static void clear_canvas(void) {
    ensure();
    for (int i = 0; i < CW * CH; i++) canvas[i] = 15;
    dirty = 1;
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
    if (e->type == EVT_MENU) {
        if (e->menu_id == 1) {
            if (!confirm_close()) return;
            clear_canvas();
            filename[0] = 0;
            dirty = 0;
            set_title();
        } else if (e->menu_id == 2) {
            if (!confirm_close()) return;
            uint16_t dir = parent;
            char name[28] = "";
            if (!gui_file("Open", 0, &dir, name, 28)) return;
            int id = fs_lookup(dir, name);
            if (id >= 0) app_paint_load(id);
        } else if (e->menu_id == 3) save_image();
        else if (e->menu_id == 4) {
            char name[28];
            strncpy(name, filename[0] ? filename : "PICTURE.GIM", 28);
            uint16_t dir = parent;
            if (!gui_file("Save As", 1, &dir, name, 28)) return;
            strncpy(filename, name, 28);
            parent = dir;
            save_image();
        } else if (e->menu_id == 5) {
            if (confirm_close()) gui_close(w);
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
            if (e->y >= 8 + 4 * 32 + 8 && e->y < 8 + 4 * 32 + 34) clear_canvas();
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
    if (!win) return;
    win->ask_close = paint_ask;
    window_add_menu(win, "File");
    window_add_item(win, 0, "New", 1);
    window_add_item(win, 0, "Open...", 2);
    window_add_item(win, 0, "Save", 3);
    window_add_item(win, 0, "Save As...", 4);
    window_add_item(win, 0, "Exit", 5);
    set_title();
    serial_write("APP: PAINT\n");
}

void app_paint_load(int inode) {
    ensure();
    app_paint_open();
    if (!win || inode <= 0 || fs_is_dir(inode)) return;
    if (!filebuf) filebuf = (uint8_t *)0x700000;
    uint32_t sz = 0;
    if (fs_read(inode, filebuf, 8u + CW * CH, &sz) || sz < 8 || memcmp(filebuf, "GIMG", 4) != 0) {
        gui_dialog("Paint", "That file is not a GeometricalOS picture.", "OK", 0, 0);
        return;
    }
    int w = filebuf[4] | (filebuf[5] << 8);
    int h = filebuf[6] | (filebuf[7] << 8);
    if (w != CW || h != CH || sz < 8u + CW * CH) {
        gui_dialog("Paint", "This picture is the wrong size.", "OK", 0, 0);
        return;
    }
    memcpy(canvas, filebuf + 8, CW * CH);
    parent = fs_parent(inode);
    strncpy(filename, fs_name(inode), 28);
    dirty = 0;
    set_title();
    gui_focus(win);
    app_recent_add(inode);
}
