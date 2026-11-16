#include "gfx.h"
#include "bootinfo.h"
#include "io.h"
#include "input.h"

static int sw, sh, pitch, bpp;
static uint8_t *lfb;
static uint32_t *back;
static int red_size, red_pos, green_size, green_pos, blue_size, blue_pos;
static int fast32;
static const uint8_t *font;
static int font_h;
static int font_fallback;
extern const uint8_t font8x16_fallback[128][16];
static int clip_x0, clip_y0, clip_x1, clip_y1;

/* 16x16 arrow: 0 transparent, 1 black, 2 white. */
static const uint8_t cursor_bits[16][16] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,1,1,1,0,0,0,0,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
};

static int cur_x = -32, cur_y = -32;
static int shown_x = -32, shown_y = -32;
static int dirty;
static int dx0, dy0, dx1, dy1;

static void mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    if (!dirty) {
        dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; dirty = 1;
        return;
    }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
}

int gfx_width(void) { return sw; }
int gfx_height(void) { return sh; }
int gfx_font_h(void) { return font_h; }

static void dispi_write(uint16_t index, uint16_t value) {
    outw(0x1CE, index);
    outw(0x1CF, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(0x1CE, index);
    return inw(0x1CF);
}

static void dispi_set_mode(int width, int height, int depth) {
    dispi_write(4, 0);
    dispi_write(1, (uint16_t)width);
    dispi_write(2, (uint16_t)height);
    dispi_write(3, (uint16_t)depth);
    dispi_write(6, (uint16_t)width);
    dispi_write(8, 0);
    dispi_write(9, 0);
    dispi_write(4, 0x41);
}

int gfx_set_display_mode(int width, int height) {
    const uint32_t max_pixels = (5u * 1024u * 1024u) / sizeof(uint32_t);
    if (width < 640 || height < 480 || width > 1280 || height > 1024 ||
        (uint32_t)width * (uint32_t)height > max_pixels)
        return 0;

    if (dispi_read(0) < 0xB0C0) return 0;

    int old_width = sw;
    int old_height = sh;
    int old_bpp = bpp;
    dispi_set_mode(width, height, 32);
    if (dispi_read(1) != (uint16_t)width ||
        dispi_read(2) != (uint16_t)height ||
        dispi_read(3) != 32 || dispi_read(4) != 0x41) {
        dispi_set_mode(old_width, old_height, old_bpp);
        return 0;
    }

    sw = width;
    sh = height;
    bpp = 32;
    pitch = width * 4;
    red_size = green_size = blue_size = 8;
    red_pos = 16;
    green_pos = 8;
    blue_pos = 0;
    fast32 = 1;
    gfx_reset_clip();
    cur_x = cur_y = -32;
    shown_x = shown_y = -64;
    dirty = 1;
    dx0 = dy0 = 0;
    dx1 = sw;
    dy1 = sh;
    return 1;
}

static uint32_t pack_color(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    r >>= (8 - red_size);
    g >>= (8 - green_size);
    b >>= (8 - blue_size);
    return (r << red_pos) | (g << green_pos) | (b << blue_pos);
}

int gfx_init(void) {
    const BootInfo *bi = bootinfo();
    if (bi->magic != BOOTINFO_MAGIC || bi->width < 640 || bi->height < 480 || bi->lfb == 0)
        return -1;
    sw = (int)bi->width;
    sh = (int)bi->height;
    pitch = (int)bi->pitch;
    bpp = (int)bi->bpp;
    lfb = (uint8_t *)(uintptr_t)bi->lfb;
    red_size = (int)bi->red_size;
    red_pos = (int)bi->red_pos;
    green_size = (int)bi->green_size;
    green_pos = (int)bi->green_pos;
    blue_size = (int)bi->blue_size;
    blue_pos = (int)bi->blue_pos;
    if (red_size == 0) {
        /* Sensible defaults if the BIOS left the masks blank. */
        if (bpp == 16) {
            red_size = 5; red_pos = 11; green_size = 6; green_pos = 5; blue_size = 5; blue_pos = 0;
        } else {
            red_size = 8; red_pos = 16; green_size = 8; green_pos = 8; blue_size = 8; blue_pos = 0;
        }
    }
    fast32 = (bpp == 32 && red_pos == 16 && green_pos == 8 && blue_pos == 0);
    font_h = (int)bi->font_height;
    if (font_h < 8 || font_h > 16) font_h = 16;
    font = (const uint8_t *)(uintptr_t)bi->font_addr;
    int any = 0;
    if (font && font_h >= 8) {
        for (int i = 0; i < 256 * font_h; i++) if (font[i]) { any = 1; break; }
    }
    font_fallback = 0;
    if (!any) {
        font = &font8x16_fallback[0][0];
        font_h = 16;
        font_fallback = 1;
    }
    back = (uint32_t *)0x200000;
    gfx_reset_clip();
    cur_x = cur_y = shown_x = shown_y = -32;
    dirty = 0;
    return 0;
}

void gfx_set_clip(int x, int y, int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    clip_x0 = x;
    clip_y0 = y;
    clip_x1 = x + w;
    clip_y1 = y + h;
    if (clip_x0 < 0) clip_x0 = 0;
    if (clip_y0 < 0) clip_y0 = 0;
    if (clip_x1 > sw) clip_x1 = sw;
    if (clip_y1 > sh) clip_y1 = sh;
}

void gfx_reset_clip(void) {
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = sw;
    clip_y1 = sh;
}

void gfx_pixel(int x, int y, uint32_t color) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    back[(uint32_t)y * (uint32_t)sw + (uint32_t)x] = color;
    mark_dirty(x, y, 1, 1);
}

void gfx_clear(uint32_t color) {
    gfx_fill(0, 0, sw, sh, color);
}

void gfx_fill(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = back + (uint32_t)yy * (uint32_t)sw;
        for (int xx = x0; xx < x1; xx++) row[xx] = color;
    }
    mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

void gfx_hline(int x, int y, int w, uint32_t color) {
    gfx_fill(x, y, w, 1, color);
}

void gfx_vline(int x, int y, int h, uint32_t color) {
    gfx_fill(x, y, 1, h, color);
}

void gfx_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint32_t color) {
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, color);
        gfx_pixel(cx + y, cy + x, color);
        gfx_pixel(cx - y, cy + x, color);
        gfx_pixel(cx - x, cy + y, color);
        gfx_pixel(cx - x, cy - y, color);
        gfx_pixel(cx - y, cy - x, color);
        gfx_pixel(cx + y, cy - x, color);
        gfx_pixel(cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_hline(cx - x, cy + y, x * 2 + 1, color);
        gfx_hline(cx - x, cy - y, x * 2 + 1, color);
        gfx_hline(cx - y, cy + x, y * 2 + 1, color);
        gfx_hline(cx - y, cy - x, y * 2 + 1, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void tri_span(int y, int xa, int xb, uint32_t color) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    gfx_hline(xa, y, xb - xa + 1, color);
}

static int edge_x(int x0, int y0, int x1, int y1, int y) {
    if (y1 == y0) return x0;
    return x0 + (x1 - x0) * (y - y0) / (y1 - y0);
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 == y0) {
        int a = x0, b = x0;
        if (x1 < a) a = x1;
        if (x1 > b) b = x1;
        if (x2 < a) a = x2;
        if (x2 > b) b = x2;
        tri_span(y0, a, b, color);
        return;
    }
    for (int y = y0; y <= y2; y++) {
        int xa = edge_x(x0, y0, x2, y2, y);
        int xb = (y < y1) ? edge_x(x0, y0, x1, y1, y) : edge_x(x1, y1, x2, y2, y);
        tri_span(y, xa, xb, color);
    }
}

void gfx_char(int x, int y, unsigned char c, uint32_t fg, uint32_t bg, int opaque) {
    if (font_fallback && c >= 128) c = ' ';
    const uint8_t *g = font + (unsigned)c * (unsigned)font_h;
    for (int row = 0; row < font_h; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            int on = bits & (0x80 >> col);
            if (on) gfx_pixel(x + col, y + row, fg);
            else if (opaque) gfx_pixel(x + col, y + row, bg);
        }
    }
}

void gfx_char_style(int x, int y, unsigned char c, uint32_t fg, uint32_t bg,
                    int opaque, int bold, int italic, int underline) {
    if (font_fallback && c >= 128) c = ' ';
    const uint8_t *g = font + (unsigned)c * (unsigned)font_h;
    for (int row = 0; row < font_h; row++) {
        uint8_t bits = g[row];
        if (bold) bits |= bits >> 1;
        int slant = italic ? (font_h - 1 - row) / 5 : 0;
        for (int col = 0; col < 8; col++) {
            int on = (bits & (0x80 >> col)) != 0;
            if (on) {
                gfx_pixel(x + col + slant, y + row, fg);
            } else if (opaque) {
                gfx_pixel(x + col + slant, y + row, bg);
            }
        }
    }
    if (underline) gfx_hline(x, y + font_h - 2, 8, fg);
}

void gfx_text(int x, int y, const char *s, uint32_t fg, uint32_t bg) {
    while (*s) {
        gfx_char(x, y, (unsigned char)*s++, fg, bg, 1);
        x += 8;
    }
}

void gfx_text_fg(int x, int y, const char *s, uint32_t fg) {
    while (*s) {
        gfx_char(x, y, (unsigned char)*s++, fg, 0, 0);
        x += 8;
    }
}

void gfx_text_scaled(int x, int y, const char *s, uint32_t fg, int scale) {
    if (scale < 1) scale = 1;
    int ox = x;
    (void)ox;
    while (*s) {
        const uint8_t *g = font + (unsigned char)*s * (unsigned)font_h;
        for (int row = 0; row < font_h; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col))
                    gfx_fill(x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
        x += 8 * scale;
        s++;
    }
}

int gfx_text_width(const char *s) {
    return (int)strlen(s) * 8;
}

void gfx_raised(int x, int y, int w, int h, uint32_t face) {
    gfx_fill(x, y, w, h, face);
    gfx_hline(x, y, w, COL_WHITE);
    gfx_vline(x, y, h, COL_WHITE);
    gfx_hline(x, y + h - 1, w, COL_BLACK);
    gfx_vline(x + w - 1, y, h, COL_BLACK);
    if (w > 2 && h > 2) {
        gfx_hline(x + 1, y + h - 2, w - 2, COL_DKGRAY);
        gfx_vline(x + w - 2, y + 1, h - 2, COL_DKGRAY);
    }
}

void gfx_sunken(int x, int y, int w, int h, uint32_t face) {
    gfx_fill(x, y, w, h, face);
    gfx_hline(x, y, w, COL_DKGRAY);
    gfx_vline(x, y, h, COL_DKGRAY);
    gfx_hline(x, y + h - 1, w, COL_WHITE);
    gfx_vline(x + w - 1, y, h, COL_WHITE);
    if (w > 2 && h > 2) {
        gfx_hline(x + 1, y + 1, w - 2, COL_BLACK);
        gfx_vline(x + 1, y + 1, h - 2, COL_BLACK);
    }
}

static struct { int x, y, w, h; } btns[48];
static int nbtns;
static int arm_on, arm_x, arm_y, arm_w, arm_h;
static int foc_on, foc_x, foc_y, foc_w, foc_h;

void gfx_buttons_begin(void) { nbtns = 0; }

static int same_btn(int x, int y, int w, int h, int x2, int y2, int w2, int h2) {
    return x == x2 && y == y2 && w == w2 && h == h2;
}

void gfx_button(int x, int y, int w, int h, const char *label, int pressed) {
    if (nbtns < 48) {
        btns[nbtns].x = x; btns[nbtns].y = y; btns[nbtns].w = w; btns[nbtns].h = h;
        nbtns++;
    }
    int inside = mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h;
    int hot = arm_on && same_btn(x, y, w, h, arm_x, arm_y, arm_w, arm_h) && inside;
    int down = pressed || hot;
    if (down) gfx_sunken(x, y, w, h, COL_GRAY);
    else gfx_raised(x, y, w, h, COL_GRAY);
    int focused = foc_on && same_btn(x, y, w, h, foc_x, foc_y, foc_w, foc_h);
    if (focused && w > 8 && h > 8)
        gfx_rect(x + 3, y + 3, w - 6, h - 6, COL_BLACK);
    if (!label) return;
    int tw = gfx_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - font_h) / 2;
    if (down) { tx++; ty++; }
    gfx_text_fg(tx, ty, label, COL_BLACK);
}

int gfx_button_hit(int x, int y, int *bx, int *by, int *bw, int *bh) {
    for (int i = nbtns - 1; i >= 0; i--) {
        if (x >= btns[i].x && x < btns[i].x + btns[i].w && y >= btns[i].y && y < btns[i].y + btns[i].h) {
            if (bx) *bx = btns[i].x;
            if (by) *by = btns[i].y;
            if (bw) *bw = btns[i].w;
            if (bh) *bh = btns[i].h;
            return 1;
        }
    }
    return 0;
}

void gfx_button_arm(int x, int y, int w, int h) {
    arm_on = 1; arm_x = x; arm_y = y; arm_w = w; arm_h = h;
    foc_on = 1; foc_x = x; foc_y = y; foc_w = w; foc_h = h;
}
void gfx_button_clear_arm(void) { arm_on = 0; }
int gfx_button_armed(void) { return arm_on; }
int gfx_button_arm_contains(int x, int y) {
    return arm_on && x >= arm_x && x < arm_x + arm_w && y >= arm_y && y < arm_y + arm_h;
}
void gfx_button_focus(int x, int y, int w, int h) {
    foc_on = 1; foc_x = x; foc_y = y; foc_w = w; foc_h = h;
}
int gfx_button_focus_rect(int *x, int *y, int *w, int *h) {
    if (!foc_on) return 0;
    if (x) *x = foc_x;
    if (y) *y = foc_y;
    if (w) *w = foc_w;
    if (h) *h = foc_h;
    return 1;
}

int gfx_button_cycle(int x, int y, int w, int h, int reverse) {
    int matches[48], count = 0, current = -1;
    for (int i = 0; i < nbtns; i++) {
        int cx = btns[i].x + btns[i].w / 2;
        int cy = btns[i].y + btns[i].h / 2;
        if (cx < x || cy < y || cx >= x + w || cy >= y + h) continue;
        if (foc_on && same_btn(btns[i].x, btns[i].y, btns[i].w, btns[i].h,
                               foc_x, foc_y, foc_w, foc_h))
            current = count;
        matches[count++] = i;
    }
    if (!count) return 0;
    int next = current < 0 ? (reverse ? count - 1 : 0) :
               (current + (reverse ? count - 1 : 1)) % count;
    int i = matches[next];
    gfx_button_focus(btns[i].x, btns[i].y, btns[i].w, btns[i].h);
    return 1;
}
void gfx_button_focus_clear(void) { foc_on = 0; }

void gfx_slider(int x, int y, int w, int val, int lo, int hi, int focused) {
    if (w < 40) w = 40;
    if (hi <= lo) hi = lo + 1;
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    int track_y = y + 8;
    gfx_sunken(x + 8, track_y, w - 16, 6, COL_WHITE);
    int span = w - 28;
    int thumb = x + 8 + (val - lo) * span / (hi - lo);
    gfx_raised(thumb, y, 14, 22, COL_GRAY);
    if (focused) gfx_rect(thumb + 2, y + 2, 10, 18, COL_BLACK);
}

int gfx_slider_value(int x, int track_x, int track_w, int lo, int hi) {
    if (hi <= lo) return lo;
    int span = track_w - 28;
    if (span < 1) span = 1;
    int rel = x - (track_x + 8);
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    return lo + rel * (hi - lo) / span;
}

void gfx_blit_indexed(int dx, int dy, const uint8_t *src, int bw, int bh,
                      const uint32_t *pal) {
    for (int y = 0; y < bh; y++) {
        int py = dy + y;
        if (py < clip_y0 || py >= clip_y1) continue;
        uint32_t *row = back + (uint32_t)py * (uint32_t)sw;
        const uint8_t *srow = src + (uint32_t)y * (uint32_t)bw;
        int x0 = 0, x1 = bw;
        if (dx < clip_x0) x0 = clip_x0 - dx;
        if (dx + bw > clip_x1) x1 = clip_x1 - dx;
        for (int x = x0; x < x1; x++) row[dx + x] = pal[srow[x] & 15];
    }
    mark_dirty(dx, dy, bw, bh);
}

void gfx_logo(int cx, int cy, int scale) {
    if (scale < 1) scale = 1;
    int s = 28 * scale;
    /* Blue square. */
    gfx_fill(cx - s, cy - s / 3, s, s, 0x2060D0);
    gfx_rect(cx - s, cy - s / 3, s, s, COL_WHITE);
    /* Yellow circle overlapping. */
    gfx_fill_circle(cx + s / 5, cy - s / 6, (18 * scale), 0xF0C020);
    gfx_circle(cx + s / 5, cy - s / 6, (18 * scale), COL_WHITE);
    /* Red triangle. */
    gfx_fill_triangle(cx - s / 2, cy + s / 2,
                      cx + s / 2, cy + s / 2,
                      cx, cy - s / 2,
                      0xD02020);
}

static void put_front(int x, int y, uint32_t rgb) {
    if ((unsigned)x >= (unsigned)sw || (unsigned)y >= (unsigned)sh) return;
    uint32_t p = pack_color(rgb);
    uint8_t *dst = lfb + (uint32_t)y * (uint32_t)pitch + (uint32_t)x * (bpp / 8);
    if (bpp == 32) {
        dst[0] = (uint8_t)p;
        dst[1] = (uint8_t)(p >> 8);
        dst[2] = (uint8_t)(p >> 16);
        dst[3] = (uint8_t)(p >> 24);
    } else if (bpp == 24) {
        dst[0] = (uint8_t)p;
        dst[1] = (uint8_t)(p >> 8);
        dst[2] = (uint8_t)(p >> 16);
    } else {
        dst[0] = (uint8_t)p;
        dst[1] = (uint8_t)(p >> 8);
    }
}

static int cursor_overlay(int x, int y, uint32_t *color) {
    int cx = x - cur_x;
    int cy = y - cur_y;
    if ((unsigned)cx >= 16u || (unsigned)cy >= 16u) return 0;
    uint8_t b = cursor_bits[cy][cx];
    if (b == 1) { *color = COL_BLACK; return 1; }
    if (b == 2) { *color = COL_WHITE; return 1; }
    return 0;
}

/* One write per pixel: the back buffer, with the cursor composited on top.
 * The front buffer is never cleared and the cursor is never erased first.
 */
static void blit_box(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = back + (uint32_t)y * (uint32_t)sw;
        for (int x = x0; x < x1; x++) {
            uint32_t c = row[x];
            uint32_t over;
            if (cursor_overlay(x, y, &over)) c = over;
            put_front(x, y, c);
        }
    }
}

void gfx_present(void) {
    if (!dirty) {
        if (shown_x != cur_x || shown_y != cur_y)
            blit_box(cur_x, cur_y, cur_x + 16, cur_y + 16);
        shown_x = cur_x;
        shown_y = cur_y;
        return;
    }
    blit_box(dx0, dy0, dx1, dy1);
    if (cur_x < dx0 || cur_y < dy0 || cur_x + 16 > dx1 || cur_y + 16 > dy1)
        blit_box(cur_x, cur_y, cur_x + 16, cur_y + 16);
    if (shown_x != cur_x || shown_y != cur_y)
        blit_box(shown_x, shown_y, shown_x + 16, shown_y + 16);
    shown_x = cur_x;
    shown_y = cur_y;
    dirty = 0;
}

void cursor_invalidate(void) {
    shown_x = shown_y = -64;
}

void cursor_restore(void) {
    /* The cursor lives in the same blit as the scene. There is no separate erase. */
}

void cursor_draw(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (sw > 0 && x > sw - 1) x = sw - 1;
    if (sh > 0 && y > sh - 1) y = sh - 1;
    if (x == cur_x && y == cur_y && shown_x == x && shown_y == y) return;
    int ox = shown_x, oy = shown_y;
    cur_x = x;
    cur_y = y;
    /* Draw the cursor in its new place first, then repair the old place.
     * Overlapping pixels stay covered because the second blit uses the new position.
     */
    blit_box(x, y, x + 16, y + 16);
    if (ox != x || oy != y) blit_box(ox, oy, ox + 16, oy + 16);
    shown_x = x;
    shown_y = y;
}
