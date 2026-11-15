#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "net.h"
#include "input.h"
#include "io.h"
#include "serial.h"

#define MAXRUN 220
struct brun {
    int x, y, w;
    char text[40];
    char href[64];
    uint8_t flags;
};

static Window *win;
static char url[120] = "http://example.com/";
static char status[80] = "Type an address and press Enter.";
static int addr_focus = 1;
static int scroll;
static int content_h;
static struct brun *runs;
static int nruns;
static char hist[6][120];
static int hist_n, hist_i;
static char *page;

static void ensure(void) {
    if (!runs) runs = (struct brun *)0x750000;
    if (!page) page = (char *)0x760000;
}

static void add_hist(const char *u) {
    if (hist_n && strcmp(hist[hist_i], u) == 0) return;
    if (hist_n < 6) hist_n++;
    hist_i = (hist_i + 1) % 6;
    strncpy(hist[hist_i], u, 120);
}

static int push_run(const char *text, const char *href, int flags) {
    if (nruns >= MAXRUN) return 0;
    struct brun *r = &runs[nruns++];
    memset(r, 0, sizeof(*r));
    strncpy(r->text, text ? text : "", 40);
    strncpy(r->href, href ? href : "", 64);
    r->flags = (uint8_t)flags;
    return 1;
}

static void html_to_runs(const char *html) {
    nruns = 0;
    int bold = 0, italic = 0, heading = 0, pre = 0;
    char href[64];
    href[0] = 0;
    int in_link = 0;
    char word[40];
    int wl = 0;
    int skip = 0;
    const char *p = html;
    while (*p && nruns < MAXRUN) {
        if (*p == '<') {
            if (wl) { word[wl] = 0; push_run(word, in_link ? href : "", (bold?1:0)|(italic?2:0)|(in_link?4:0)|(heading?8:0)|(pre?16:0)); wl = 0; }
            char tag[24];
            int t = 0;
            int close = 0;
            p++;
            if (*p == '/') { close = 1; p++; }
            while (*p && *p != '>' && *p != ' ' && t < 22) tag[t++] = (char)((*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : *p), p++;
            tag[t] = 0;
            char attr[64];
            attr[0] = 0;
            if (!close && strcmp(tag, "a") == 0) {
                const char *h = p;
                while (*h && *h != '>') h++;
                const char *q = p;
                while (q + 6 < h) {
                    if (strncasecmp(q, "href=", 5) == 0) {
                        q += 5;
                        char quote = 0;
                        if (*q == '"' || *q == '\'') quote = *q++;
                        int k = 0;
                        while (*q && *q != '>' && *q != quote && k < 63) attr[k++] = *q++;
                        attr[k] = 0;
                        break;
                    }
                    q++;
                }
            }
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            if (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0) { skip = !close; continue; }
            if (skip) continue;
            if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0) bold = !close;
            else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0) italic = !close;
            else if (strcmp(tag, "pre") == 0) pre = !close;
            else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '3' && !tag[2]) heading = !close;
            else if (strcmp(tag, "a") == 0) {
                if (!close) { in_link = 1; strncpy(href, attr, 64); }
                else { in_link = 0; href[0] = 0; }
            } else if (!close && (strcmp(tag, "br") == 0 || strcmp(tag, "p") == 0 || strcmp(tag, "div") == 0 ||
                                  strcmp(tag, "li") == 0 || strcmp(tag, "tr") == 0 || tag[0] == 'h')) {
                push_run("", "", 32);
            }
            continue;
        }
        if (skip) { p++; continue; }
        char ch = *p++;
        if (ch == '&') {
            if (strncmp(p, "amp;", 4) == 0) { ch = '&'; p += 4; }
            else if (strncmp(p, "lt;", 3) == 0) { ch = '<'; p += 3; }
            else if (strncmp(p, "gt;", 3) == 0) { ch = '>'; p += 3; }
            else if (strncmp(p, "quot;", 5) == 0) { ch = '"'; p += 5; }
            else if (strncmp(p, "nbsp;", 5) == 0) { ch = ' '; p += 5; }
            else { while (*p && *p != ';') p++; if (*p == ';') p++; ch = '?'; }
        }
        if (ch == '\r') continue;
        if (!pre && (ch == '\n' || ch == '\t')) ch = ' ';
        if (!pre && ch == ' ' && wl == 0) continue;
        if (ch == '\n' || (ch == ' ' && !pre)) {
            if (wl) { word[wl] = 0; push_run(word, in_link ? href : "", (bold?1:0)|(italic?2:0)|(in_link?4:0)|(heading?8:0)|(pre?16:0)); wl = 0; }
            if (ch == '\n') push_run("", "", 32);
            continue;
        }
        if (wl < 39) word[wl++] = ch;
        else {
            word[wl] = 0;
            push_run(word, in_link ? href : "", (bold?1:0)|(italic?2:0)|(in_link?4:0)|(heading?8:0)|(pre?16:0));
            wl = 0;
            word[wl++] = ch;
        }
    }
    if (wl) { word[wl] = 0; push_run(word, in_link ? href : "", (bold?1:0)|(italic?2:0)|(in_link?4:0)|(heading?8:0)); }
}

static void layout(int width) {
    int x = 4, y = 4;
    int fh = gfx_font_h();
    if (width < 40) width = 40;
    for (int i = 0; i < nruns; i++) {
        struct brun *r = &runs[i];
        if (r->flags & 32) { x = 4; y += fh + 4; r->x = x; r->y = y; r->w = 0; continue; }
        int tw = (int)strlen(r->text) * 8 + 8;
        if (r->flags & 8) tw += 8;
        if (x > 4 && x + tw > width) { x = 4; y += fh + 2; }
        r->x = x;
        r->y = y;
        r->w = tw;
        x += tw;
    }
    content_h = y + fh + 8;
}

static void go(const char *target) {
    ensure();
    strncpy(url, target, sizeof(url));
    url[sizeof(url) - 1] = 0;
    strcpy(status, "Loading...");
    int len = 0, code = 0;
    char finalu[120];
    finalu[0] = 0;
    int rc = http_get(url, page, 32 * 1024, &len, &code, finalu, sizeof(finalu));
    if (finalu[0]) strncpy(url, finalu, sizeof(url));
    if (rc == -2) strcpy(status, "HTTPS is not supported. Use http://");
    else if (rc == -4) strcpy(status, "Could not find that name.");
    else if (rc == -5) strcpy(status, "Could not connect.");
    else if (rc != 0) strcpy(status, "The page did not load.");
    else {
        char b[40];
        strcpy(status, "Loaded ");
        int o = 7;
        int v = code, revn = 0; char rev[8];
        if (!v) rev[revn++] = '0';
        while (v) { rev[revn++] = (char)('0' + v % 10); v /= 10; }
        while (revn) b[0] = rev[--revn], status[o++] = b[0];
        status[o] = 0;
        html_to_runs(page);
        add_hist(url);
        scroll = 0;
    }
    if (rc != 0) { nruns = 0; push_run(status, "", 0); }
}

static void browser_event(Window *w, Event *e) {
    ensure();
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int fh = gfx_font_h();
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_button(cx + 4, cy + 4, 46, 22, "Back", 0);
        gfx_button(cx + 54, cy + 4, 46, 22, "Fwd", 0);
        gfx_button(cx + 104, cy + 4, 58, 22, "Reload", 0);
        gfx_sunken(cx + 168, cy + 4, cw - 230, 22, addr_focus ? COL_WHITE : COL_GRAY);
        gfx_text(cx + 172, cy + 6, url, COL_BLACK, addr_focus ? COL_WHITE : COL_GRAY);
        gfx_button(cx + cw - 54, cy + 4, 48, 22, "Go", 0);
        int vy = cy + 32;
        int vh = ch - 52;
        gfx_sunken(cx + 2, vy, cw - 4, vh, COL_WHITE);
        gfx_set_clip(cx + 4, vy + 2, cw - 8, vh - 4);
        layout(cw - 16);
        for (int i = 0; i < nruns; i++) {
            struct brun *r = &runs[i];
            int y = vy + 4 + r->y - scroll;
            if (y < vy || y > vy + vh) continue;
            if (!r->text[0]) continue;
            uint32_t fg = (r->flags & 4) ? COL_BLUE : ((r->flags & 8) ? COL_NAVY : COL_BLACK);
            if (r->flags & 2) fg = 0x404080;
            gfx_text(cx + 8 + r->x, y, r->text, fg, COL_WHITE);
            if (r->flags & 1) gfx_text(cx + 9 + r->x, y, r->text, fg, COL_WHITE);
            if (r->flags & 4) gfx_hline(cx + 8 + r->x, y + fh - 1, (int)strlen(r->text) * 8, COL_BLUE);
        }
        gfx_reset_clip();
        gfx_text(cx + 6, cy + ch - 16, status, COL_BLACK, COL_GRAY);
        return;
    }
    if (e->type == EVT_KEY) {
        if (!addr_focus) {
            if (e->code == KEY_UP) scroll -= 24;
            else if (e->code == KEY_DOWN || e->code == KEY_PGDN) scroll += e->code == KEY_PGDN ? 80 : 24;
            else if (e->code == KEY_PGUP) scroll -= 80;
            if (scroll < 0) scroll = 0;
            if (scroll > content_h) scroll = content_h;
            return;
        }
        if (e->code == 10 || e->code == 13) { go(url); return; }
        if (e->code == 8 && strlen(url)) url[strlen(url) - 1] = 0;
        else if (e->code >= 32 && e->code < 127 && strlen(url) < 110) {
            int n = (int)strlen(url);
            url[n] = (char)e->code;
            url[n + 1] = 0;
        }
        return;
    }
    if (e->type != EVT_DOWN) return;
    if (e->y < 28) {
        addr_focus = 0;
        if (e->x >= 4 && e->x < 50 && hist_n && hist_i > 0) {
            hist_i = (hist_i + 6 - 1) % 6;
            go(hist[hist_i]);
        } else if (e->x >= 54 && e->x < 100 && hist_i + 1 < hist_n) {
            hist_i = (hist_i + 1) % 6;
            go(hist[hist_i]);
        } else if (e->x >= 104 && e->x < 162) go(url);
        else if (e->x >= cw - 54) go(url);
        else { addr_focus = 1; }
        return;
    }
    addr_focus = 0;
    int ly = e->y - 36 + scroll;
    for (int i = 0; i < nruns; i++) {
        struct brun *r = &runs[i];
        if (!(r->flags & 4) || !r->href[0]) continue;
        if (e->x >= r->x && e->x < r->x + r->w && ly >= r->y && ly < r->y + gfx_font_h() + 2) {
            if (strncmp(r->href, "https://", 8) == 0) {
                strcpy(status, "HTTPS is not supported. Use http://");
                return;
            }
            if (strncmp(r->href, "http://", 7) == 0) go(r->href);
            else if (r->href[0] == '/') {
                char next[120];
                strncpy(next, url, 120);
                char *slash = next + 7;
                while (*slash && *slash != '/') slash++;
                *slash = 0;
                strncpy(slash, r->href, (size_t)(next + 119 - slash));
                go(next);
            }
            return;
        }
    }
}

void app_browser_open(void) {
    ensure();
    if (win) { gui_restore(win); return; }
    int ww = 640, wh = 460;
    if (ww > gfx_width() - 10) ww = gfx_width() - 10;
    if (wh > gfx_height() - 10) wh = gfx_height() - 10;
    win = gui_create("Web Browser", ww, wh, ICON_WEB, browser_event, &win);
    serial_write("APP: BROWSER\n");
}
