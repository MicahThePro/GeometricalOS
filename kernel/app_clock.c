#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "rtc.h"
#include "serial.h"

static Window *win;
static const int SIN[91] = {
    0,4,9,13,18,22,27,31,36,40,44,49,53,58,62,66,71,75,79,83,88,92,96,100,104,108,112,116,120,124,128,132,136,139,143,147,150,154,158,161,165,168,171,175,178,181,184,187,190,193,196,199,202,204,207,210,212,215,217,219,222,224,226,228,230,232,234,236,237,239,241,242,243,245,246,247,248,249,250,251,252,253,254,254,255,255,255,256,256,256,256
};

static int isin(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return SIN[deg];
    if (deg <= 180) return SIN[180 - deg];
    if (deg <= 270) return -SIN[deg - 180];
    return -SIN[360 - deg];
}
static int icos(int deg) { return isin(deg + 90); }

static const char *WDAY[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char *MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

static void put2(char *d, int v) {
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

static void clock_event(Window *w, Event *e) {
    if (e->type != EVT_PAINT) return;
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    gfx_fill(cx, cy, cw, ch, COL_GRAY);
    RtcTime t;
    rtc_read(&t);
    int ox = cx + cw / 2;
    int oy = cy + 100;
    int rad = 78;
    gfx_fill_circle(ox, oy, rad, COL_WHITE);
    gfx_circle(ox, oy, rad, COL_BLACK);
    gfx_circle(ox, oy, rad - 1, COL_DKGRAY);
    for (int i = 0; i < 12; i++) {
        int a = i * 30;
        int x1 = ox + isin(a) * (rad - 8) / 256;
        int y1 = oy - icos(a) * (rad - 8) / 256;
        int x2 = ox + isin(a) * (rad - 16) / 256;
        int y2 = oy - icos(a) * (rad - 16) / 256;
        gfx_line(x1, y1, x2, y2, COL_BLACK);
    }
    int ha = (t.hour % 12) * 30 + t.min / 2;
    int ma = t.min * 6;
    int sa = t.sec * 6;
    gfx_line(ox, oy, ox + isin(ha) * 40 / 256, oy - icos(ha) * 40 / 256, COL_BLACK);
    gfx_line(ox, oy, ox + isin(ma) * 58 / 256, oy - icos(ma) * 58 / 256, COL_NAVY);
    gfx_line(ox, oy, ox + isin(sa) * 62 / 256, oy - icos(sa) * 62 / 256, COL_RED);
    gfx_fill_circle(ox, oy, 3, COL_BLACK);

    char buf[40];
    int h12 = t.hour % 12; if (h12 == 0) h12 = 12;
    put2(buf, h12);
    buf[2] = ':';
    put2(buf + 3, t.min);
    buf[5] = ':';
    put2(buf + 6, t.sec);
    buf[8] = ' ';
    buf[9] = (t.hour < 12) ? 'A' : 'P';
    buf[10] = 'M';
    buf[11] = 0;
    gfx_text(cx + (cw - gfx_text_width(buf)) / 2, oy + rad + 16, buf, COL_BLACK, COL_GRAY);

    const char *wd = (t.wday >= 0 && t.wday <= 6) ? WDAY[t.wday] : "";
    const char *mo = (t.month >= 1 && t.month <= 12) ? MON[t.month - 1] : "";
    char date[40];
    int n = 0;
    for (int i = 0; wd[i] && n < 20; i++) date[n++] = wd[i];
    date[n++] = ' ';
    date[n++] = (char)('0' + (t.day / 10) % 10);
    date[n++] = (char)('0' + t.day % 10);
    date[n++] = ' ';
    for (int i = 0; mo[i] && n < 32; i++) date[n++] = mo[i];
    date[n++] = ' ';
    int y = t.year;
    char yb[8]; int yn = 0;
    if (y == 0) yb[yn++] = '0';
    while (y && yn < 8) { yb[yn++] = (char)('0' + y % 10); y /= 10; }
    while (yn) date[n++] = yb[--yn];
    date[n] = 0;
    gfx_text(cx + (cw - gfx_text_width(date)) / 2, oy + rad + 36, date, COL_NAVY, COL_GRAY);
}

void app_clock_open(void) {
    if (win) { gui_restore(win); return; }
    win = gui_create("Clock", 280, 320, ICON_CLOCK, clock_event, &win);
    serial_write("APP: CLOCK\n");
}
