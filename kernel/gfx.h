#ifndef GOS_GFX_H
#define GOS_GFX_H

#include <stdint.h>

#define COL_TEAL      0x008080u
#define COL_GRAY      0xC0C0C0u
#define COL_NAVY      0x000080u
#define COL_WHITE     0xFFFFFFu
#define COL_BLACK     0x000000u
#define COL_DKGRAY    0x808080u
#define COL_BLUE      0x0000AAu
#define COL_LTBLUE    0x0000FFu
#define COL_YELLOW    0xFFFF00u
#define COL_RED       0xFF0000u
#define COL_GREEN     0x008000u
#define COL_LTGREEN   0x00FF00u
#define COL_MAROON    0x800000u
#define COL_PURPLE    0x800080u
#define COL_CYAN      0x00FFFFu
#define COL_SETUPBLUE 0x000082u
#define COL_GEO_DARK  0x10182Bu
#define COL_GEO_MID   0x1A2A45u
#define COL_GEO_PALE  0xE8D6AEu
#define COL_AMBER     0xD9A441u

int gfx_init(void);
int gfx_width(void);
int gfx_height(void);
int gfx_font_h(void);
int gfx_set_display_mode(int width, int height);

void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

void gfx_clear(uint32_t color);
void gfx_pixel(int x, int y, uint32_t color);
void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_hline(int x, int y, int w, uint32_t color);
void gfx_vline(int x, int y, int h, uint32_t color);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_circle(int cx, int cy, int r, uint32_t color);
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);

void gfx_char(int x, int y, unsigned char c, uint32_t fg, uint32_t bg, int opaque);
void gfx_char_style(int x, int y, unsigned char c, uint32_t fg, uint32_t bg,
                    int opaque, int bold, int italic, int underline);
void gfx_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void gfx_text_fg(int x, int y, const char *s, uint32_t fg);
void gfx_text_scaled(int x, int y, const char *s, uint32_t fg, int scale);
int gfx_text_width(const char *s);

void gfx_raised(int x, int y, int w, int h, uint32_t face);
void gfx_sunken(int x, int y, int w, int h, uint32_t face);
void gfx_button(int x, int y, int w, int h, const char *label, int pressed);
void gfx_buttons_begin(void);
int gfx_button_hit(int x, int y, int *bx, int *by, int *bw, int *bh);
void gfx_button_arm(int x, int y, int w, int h);
void gfx_button_clear_arm(void);
int gfx_button_armed(void);
int gfx_button_arm_contains(int x, int y);
void gfx_button_focus(int x, int y, int w, int h);
int gfx_button_focus_rect(int *x, int *y, int *w, int *h);
int gfx_button_cycle(int x, int y, int w, int h, int reverse);
void gfx_button_focus_clear(void);
void gfx_slider(int x, int y, int w, int val, int lo, int hi, int focused);
int gfx_slider_value(int x, int track_x, int track_w, int lo, int hi);

void gfx_logo(int cx, int cy, int scale);
void gfx_blit_indexed(int dx, int dy, const uint8_t *src, int bw, int bh,
                      const uint32_t *pal);
void gfx_present(void);

/* Cursor is drawn on the front buffer after present. */
void cursor_invalidate(void);
void cursor_draw(int x, int y);
void cursor_restore(void);

#endif
