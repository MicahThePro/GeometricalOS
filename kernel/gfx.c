#include "gfx.h"
#include "bootinfo.h"
#include "io.h"

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

static int cur_saved;
static int cur_sx, cur_sy;
static uint8_t cur_bg[16 * 16 * 4];

int gfx_width(void) { return sw; }
int gfx_height(void) { return sh; }
int gfx_font_h(void) { return font_h; }

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
    cur_saved = 0;
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

void gfx_button(int x, int y, int w, int h, const char *label, int pressed) {
    if (pressed) gfx_sunken(x, y, w, h, COL_GRAY);
    else gfx_raised(x, y, w, h, COL_GRAY);
    if (!label) return;
    int tw = gfx_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - font_h) / 2;
    if (pressed) { tx++; ty++; }
    gfx_text_fg(tx, ty, label, COL_BLACK);
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

static uint32_t get_front_raw(int x, int y, uint8_t *raw4) {
    uint8_t *src = lfb + (uint32_t)y * (uint32_t)pitch + (uint32_t)x * (bpp / 8);
    int n = bpp / 8;
    for (int i = 0; i < 4; i++) raw4[i] = (i < n) ? src[i] : 0;
    return 0;
}

static void put_front_raw(int x, int y, const uint8_t *raw4) {
    uint8_t *dst = lfb + (uint32_t)y * (uint32_t)pitch + (uint32_t)x * (bpp / 8);
    int n = bpp / 8;
    for (int i = 0; i < n; i++) dst[i] = raw4[i];
}

void gfx_present(void) {
    cursor_restore();
    if (fast32 && pitch == sw * 4) {
        uint32_t *d = (uint32_t *)lfb;
        uint32_t n = (uint32_t)sw * (uint32_t)sh;
        for (uint32_t i = 0; i < n; i++) d[i] = back[i];
        return;
    }
    for (int y = 0; y < sh; y++) {
        uint32_t *row = back + (uint32_t)y * (uint32_t)sw;
        if (bpp == 32) {
            uint8_t *dst = lfb + (uint32_t)y * (uint32_t)pitch;
            for (int x = 0; x < sw; x++) {
                uint32_t p = fast32 ? row[x] : pack_color(row[x]);
                dst[x * 4 + 0] = (uint8_t)p;
                dst[x * 4 + 1] = (uint8_t)(p >> 8);
                dst[x * 4 + 2] = (uint8_t)(p >> 16);
                dst[x * 4 + 3] = (uint8_t)(p >> 24);
            }
        } else if (bpp == 24) {
            uint8_t *dst = lfb + (uint32_t)y * (uint32_t)pitch;
            for (int x = 0; x < sw; x++) {
                uint32_t p = pack_color(row[x]);
                dst[x * 3 + 0] = (uint8_t)p;
                dst[x * 3 + 1] = (uint8_t)(p >> 8);
                dst[x * 3 + 2] = (uint8_t)(p >> 16);
            }
        } else {
            uint8_t *dst = lfb + (uint32_t)y * (uint32_t)pitch;
            for (int x = 0; x < sw; x++) {
                uint32_t p = pack_color(row[x]);
                dst[x * 2 + 0] = (uint8_t)p;
                dst[x * 2 + 1] = (uint8_t)(p >> 8);
            }
        }
    }
}

void cursor_invalidate(void) {
    cur_saved = 0;
}

void cursor_restore(void) {
    if (!cur_saved) return;
    for (int y = 0; y < 16; y++) {
        int py = cur_sy + y;
        if ((unsigned)py >= (unsigned)sh) continue;
        for (int x = 0; x < 16; x++) {
            int px = cur_sx + x;
            if ((unsigned)px >= (unsigned)sw) continue;
            put_front_raw(px, py, &cur_bg[(y * 16 + x) * 4]);
        }
    }
    cur_saved = 0;
}

void cursor_draw(int x, int y) {
    cursor_restore();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > sw - 1) x = sw - 1;
    if (y > sh - 1) y = sh - 1;
    cur_sx = x;
    cur_sy = y;
    for (int cy = 0; cy < 16; cy++) {
        int py = y + cy;
        for (int cx = 0; cx < 16; cx++) {
            int px = x + cx;
            uint8_t *slot = &cur_bg[(cy * 16 + cx) * 4];
            if ((unsigned)px < (unsigned)sw && (unsigned)py < (unsigned)sh)
                get_front_raw(px, py, slot);
            else
                slot[0] = slot[1] = slot[2] = slot[3] = 0;
            uint8_t b = cursor_bits[cy][cx];
            if (b == 1) put_front(px, py, COL_BLACK);
            else if (b == 2) put_front(px, py, COL_WHITE);
        }
    }
    cur_saved = 1;
}
