#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "io.h"
#include "serial.h"

#define SCALE 1000

static Window *win;
static int64_t acc;
static int64_t entry_v;
static int entry_on;
static int entry_dec; /* digits after dot, -1 if none */
static int op;        /* 0 none, 1+ 2- 3* 4/ */
static int error;
static char display[24];

static void format_value(int64_t v, char *out) {
    int neg = v < 0;
    if (neg) v = -v;
    int64_t ip = v / SCALE;
    int frac = (int)(v % SCALE);
    char tmp[24];
    int n = 0;
    if (ip == 0) tmp[n++] = '0';
    else {
        char rev[16];
        int r = 0;
        while (ip && r < 16) { rev[r++] = (char)('0' + (ip % 10)); ip /= 10; }
        while (r) tmp[n++] = rev[--r];
    }
    if (frac) {
        tmp[n++] = '.';
        int div = 100;
        for (int i = 0; i < 3; i++) {
            int d = frac / div;
            tmp[n++] = (char)('0' + d);
            frac %= div;
            div /= 10;
        }
        while (n && tmp[n - 1] == '0') n--;
        if (n && tmp[n - 1] == '.') n--;
    }
    int p = 0;
    if (neg) out[p++] = '-';
    for (int i = 0; i < n && p < 20; i++) out[p++] = tmp[i];
    out[p] = 0;
}

static void format_entry(void) {
    format_value(entry_v, display);
    if (entry_dec == 0) {
        int n = (int)strlen(display);
        if (n < 20) { display[n++] = '.'; display[n] = 0; }
    }
}

static void refresh_display(void) {
    if (error) { strcpy(display, "Error"); return; }
    if (entry_on) format_entry();
    else format_value(acc, display);
}

static int64_t apply(int64_t a, int which, int64_t b) {
    if (which == 1) return a + b;
    if (which == 2) return a - b;
    if (which == 3) {
        int neg = (a < 0) ^ (b < 0);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        int64_t hi = a / SCALE;
        int64_t lo = a % SCALE;
        int64_t r = hi * b + (lo * b) / SCALE;
        return neg ? -r : r;
    }
    if (which == 4) {
        if (b == 0) { error = 1; return 0; }
        int neg = (a < 0) ^ (b < 0);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        int64_t r = (a * SCALE) / b;
        return neg ? -r : r;
    }
    return b;
}

static void push_digit(int d) {
    if (error) { error = 0; acc = 0; op = 0; }
    if (!entry_on) {
        entry_on = 1;
        entry_v = 0;
        entry_dec = -1;
    }
    if (entry_dec < 0) {
        if (entry_v / SCALE > 9999999) return;
        entry_v = entry_v * 10 + (int64_t)d * SCALE;
    } else if (entry_dec < 3) {
        int mul = 100;
        for (int i = 0; i < entry_dec; i++) mul /= 10;
        entry_v += (int64_t)d * mul;
        entry_dec++;
    }
}

static void commit_op(int next) {
    if (error) return;
    int64_t cur = entry_on ? entry_v : acc;
    if (op) acc = apply(acc, op, cur);
    else acc = cur;
    entry_on = 0;
    entry_dec = -1;
    op = next;
}

static void press(int id) {
    if (id >= 0 && id <= 9) push_digit(id);
    else if (id == 10) { /* dot */
        if (!entry_on) { entry_on = 1; entry_v = 0; entry_dec = 0; }
        else if (entry_dec < 0) entry_dec = 0;
    } else if (id == 11) { /* C */
        acc = 0; entry_v = 0; entry_on = 0; entry_dec = -1; op = 0; error = 0;
    } else if (id == 12) { /* +/- */
        if (entry_on) entry_v = -entry_v;
        else acc = -acc;
    } else if (id == 13) { /* back */
        if (entry_on) {
            if (entry_dec > 0) {
                int mul = 1;
                for (int i = 0; i < 3 - entry_dec; i++) mul *= 10;
                int64_t ip = entry_v / SCALE;
                int frac = (int)(entry_v % SCALE);
                frac = (frac / (mul * 10)) * (mul * 10);
                entry_v = ip * SCALE + frac;
                entry_dec--;
                if (entry_dec == 0) entry_dec = -1;
            } else {
                entry_v = (entry_v / SCALE) / 10 * SCALE;
            }
        }
    } else if (id >= 14 && id <= 17) commit_op(id - 13);
    else if (id == 18) { commit_op(0); op = 0; }
    refresh_display();
}

static void calc_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int gap = 4;
    int bw = (cw - 16 - gap * 3) / 4;
    if (bw < 28) bw = 28;
    int bh = 28;
    int x0 = cx + 8;
    int y0 = cy + 48;
    static const char *labels[] = {
        "C", "+/-", "BS", "/",
        "7", "8", "9", "*",
        "4", "5", "6", "-",
        "1", "2", "3", "+",
        "0", ".", "=", "+"
    };
    static const int ids[] = {
        11, 12, 13, 17,
        7, 8, 9, 16,
        4, 5, 6, 15,
        1, 2, 3, 14,
        0, 10, 18, 14
    };
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_sunken(cx + 8, cy + 8, cw - 16, 32, COL_WHITE);
        refresh_display();
        int tw = gfx_text_width(display);
        gfx_text(cx + cw - 16 - tw, cy + 16, display, COL_BLACK, COL_WHITE);
        for (int i = 0; i < 20; i++) {
            int c = i % 4, r = i / 4;
            int x = x0 + c * (bw + gap);
            int y = y0 + r * (bh + gap);
            if (i == 19) continue; /* duplicate + hidden; wide = is index 18 */
            int ww = bw;
            if (i == 16) { /* 0 is normal */ }
            gfx_button(x, y, ww, bh, labels[i], 0);
        }
        return;
    }
    if (e->type == EVT_DOWN || (e->type == EVT_KEY)) {
        if (e->type == EVT_DOWN) {
            for (int i = 0; i < 20; i++) {
                if (i == 19) continue;
                int c = i % 4, r = i / 4;
                int x = 8 + c * (bw + gap);
                int y = 48 + r * (bh + gap);
                if (e->x >= x && e->x < x + bw && e->y >= y && e->y < y + bh) {
                    press(ids[i]);
                    return;
                }
            }
        } else {
            int c = e->code;
            if (c >= '0' && c <= '9') press(c - '0');
            else if (c == '+') press(14);
            else if (c == '-') press(15);
            else if (c == '*') press(16);
            else if (c == '/') press(17);
            else if (c == '=' || c == 10 || c == 13) press(18);
            else if (c == '.') press(10);
            else if (c == 8) press(13);
            else if (c == 27 || c == 'c' || c == 'C') press(11);
        }
    }
}

void app_calc_open(void) {
    if (win) { gui_restore(win); return; }
    refresh_display();
    win = gui_create("Calculator", 248, 280, ICON_CALC, calc_event, &win);
    serial_write("APP: CALC\n");
}
