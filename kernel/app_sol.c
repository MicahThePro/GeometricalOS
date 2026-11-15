#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "rtc.h"
#include "input.h"
#include "io.h"
#include "serial.h"

/* Klondike, draw-one. Cards are 0..51: suit = card/13, rank = card%13 (Ace=0). */

#define CW 54
#define CH 72

static Window *win;
static uint8_t tab[7][20];
static uint8_t tabn[7];
static uint8_t tabup[7];
static uint8_t fnd[4];
static int fnd_suit[4];
static uint8_t stock[24];
static int stockn;
static uint8_t waste[24];
static int wasten;
static int sel; /* -1 none, -2 waste, else tableau pile */
static int sel_i;
static uint32_t rng = 1;

static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}
static int rankof(int c) { return c % 13; }
static int suitof(int c) { return c / 13; }
static int red(int c) { return suitof(c) < 2; }

static void new_game(void) {
    rng ^= (uint32_t)rtc_seconds_of_day() | 1u;
    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int p = 0;
    for (int col = 0; col < 7; col++) {
        tabn[col] = (uint8_t)(col + 1);
        tabup[col] = (uint8_t)col;
        for (int r = 0; r <= col; r++) tab[col][r] = deck[p++];
    }
    stockn = 0;
    wasten = 0;
    while (p < 52) stock[stockn++] = deck[p++];
    for (int i = 0; i < 4; i++) { fnd[i] = 0; fnd_suit[i] = -1; }
    sel = -1;
}

static void draw_card(int x, int y, int card, int up, int on) {
    if (!up) {
        gfx_raised(x, y, CW, CH, COL_NAVY);
        gfx_rect(x + 6, y + 6, CW - 12, CH - 12, COL_LTBLUE);
        return;
    }
    gfx_raised(x, y, CW, CH, on ? COL_YELLOW : COL_WHITE);
    static const char *ranks = "A23456789TJQK";
    static const char *suits = "HDCS";
    char lab[3] = {ranks[rankof(card)], suits[suitof(card)], 0};
    uint32_t col = red(card) ? COL_RED : COL_BLACK;
    gfx_text(x + 4, y + 4, lab, col, on ? COL_YELLOW : COL_WHITE);
    gfx_text(x + 18, y + 28, lab + 1, col, on ? COL_YELLOW : COL_WHITE);
}

static int can_found(int card) {
    int s = suitof(card), r = rankof(card);
    for (int i = 0; i < 4; i++) {
        if (fnd[i] == 0 && r == 0) return i;
        if (fnd[i] && fnd_suit[i] == s && r == fnd[i]) return i;
    }
    return -1;
}

static int top_tab(int col) {
    return tabn[col] ? tab[col][tabn[col] - 1] : -1;
}

static void flip_last(int col) {
    if (tabn[col] && tabup[col] >= tabn[col]) tabup[col] = (uint8_t)(tabn[col] - 1);
}

static int sequence_ok(int col, int idx) {
    if (idx < tabup[col] || idx >= tabn[col]) return 0;
    for (int i = idx; i + 1 < tabn[col]; i++) {
        int a = tab[col][i], b = tab[col][i + 1];
        if (red(a) == red(b) || rankof(a) != rankof(b) + 1) return 0;
    }
    return 1;
}

static void take_to_found(int card) {
    int slot = can_found(card);
    if (slot < 0) return;
    if (fnd[slot] == 0) fnd_suit[slot] = suitof(card);
    fnd[slot] = (uint8_t)(rankof(card) + 1);
}

static int try_drop_tab(int col) {
    int card, count, src;
    if (sel == -2) {
        if (!wasten) return 0;
        card = waste[wasten - 1];
        count = 1;
        src = -2;
    } else if (sel >= 0) {
        if (!sequence_ok(sel, sel_i)) return 0;
        card = tab[sel][sel_i];
        count = tabn[sel] - sel_i;
        src = sel;
        if (src == col) return 0;
    } else return 0;
    int dest = top_tab(col);
    int ok = 0;
    if (dest < 0) ok = rankof(card) == 12;
    else ok = red(card) != red(dest) && rankof(card) + 1 == rankof(dest);
    if (!ok) return 0;
    for (int i = 0; i < count; i++) {
        int c = (src == -2) ? waste[wasten - 1] : tab[src][sel_i + i];
        tab[col][tabn[col]++] = (uint8_t)c;
    }
    if (src == -2) wasten--;
    else {
        tabn[src] = (uint8_t)sel_i;
        flip_last(src);
    }
    sel = -1;
    return 1;
}

static int try_drop_found(int slot) {
    int card;
    if (sel == -2) {
        if (!wasten) return 0;
        card = waste[wasten - 1];
    } else if (sel >= 0 && sel_i == tabn[sel] - 1) {
        card = tab[sel][sel_i];
    } else return 0;
    int want = can_found(card);
    if (want != slot) return 0;
    take_to_found(card);
    if (sel == -2) wasten--;
    else {
        tabn[sel]--;
        flip_last(sel);
    }
    sel = -1;
    return 1;
}

static void auto_found(void) {
    int card = -1;
    if (sel == -2 && wasten) card = waste[wasten - 1];
    else if (sel >= 0 && sel_i == tabn[sel] - 1) card = tab[sel][sel_i];
    int slot = card >= 0 ? can_found(card) : -1;
    if (slot >= 0) try_drop_found(slot);
}

static void draw_stock(void) {
    if (stockn) {
        waste[wasten++] = stock[--stockn];
    } else {
        while (wasten) stock[stockn++] = waste[--wasten];
    }
    sel = -1;
}

static void sol_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int x0 = cx + 8, y0 = cy + 8;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, 0x006030);
        if (stockn) draw_card(x0, y0, 0, 0, 0);
        else gfx_sunken(x0, y0, CW, CH, 0x004020);
        if (wasten) draw_card(x0 + 64, y0, waste[wasten - 1], 1, sel == -2);
        for (int i = 0; i < 4; i++) {
            int x = x0 + 200 + i * 62;
            if (fnd[i]) draw_card(x, y0, fnd_suit[i] * 13 + (fnd[i] - 1), 1, 0);
            else gfx_sunken(x, y0, CW, CH, 0x004020);
        }
        for (int col = 0; col < 7; col++) {
            int x = x0 + col * 78;
            if (!tabn[col]) {
                gfx_rect(x, y0 + 90, CW, CH, 0x20C080);
                continue;
            }
            for (int r = 0; r < tabn[col]; r++) {
                int y = y0 + 90 + r * 16;
                int up = r >= tabup[col];
                int on = sel == col && up && r >= sel_i;
                if (r == tabn[col] - 1)
                    draw_card(x, y, tab[col][r], up, on);
                else if (!up) {
                    gfx_fill(x, y, CW, 16, COL_NAVY);
                    gfx_rect(x, y, CW, 16, COL_BLACK);
                } else {
                    gfx_fill(x, y, CW, 16, on ? COL_YELLOW : COL_WHITE);
                    gfx_rect(x, y, CW, 16, COL_BLACK);
                    char lab[2] = {"A23456789TJQK"[rankof(tab[col][r])], 0};
                    gfx_text(x + 4, y, lab, red(tab[col][r]) ? COL_RED : COL_BLACK,
                             on ? COL_YELLOW : COL_WHITE);
                }
            }
        }
        gfx_text(cx + 8, cy + ch - 18, "Click the deck. Double-click a card to file it.", COL_WHITE, 0x006030);
        return;
    }
    if (e->type == EVT_KEY && (e->code == 'n' || e->code == 'N')) { new_game(); return; }
    if (e->type != EVT_DOWN && e->type != EVT_RDOWN) return;
    int mx = e->x, my = e->y;
    /* stock */
    if (mx >= 8 && mx < 8 + CW && my >= 8 && my < 8 + CH) { draw_stock(); return; }
    /* waste */
    if (mx >= 72 && mx < 72 + CW && my >= 8 && my < 8 + CH && wasten) {
        if (sel == -2 && e->dbl) auto_found();
        else { sel = -2; sel_i = 0; }
        return;
    }
    /* foundations */
    for (int i = 0; i < 4; i++) {
        int x = 200 + i * 62;
        if (mx >= x && mx < x + CW && my >= 8 && my < 8 + CH) {
            try_drop_found(i);
            return;
        }
    }
    for (int col = 0; col < 7; col++) {
        int x = 8 + col * 78;
        int top = 90;
        int bottom = top + (tabn[col] ? (tabn[col] - 1) * 16 + CH : CH);
        if (mx < x || mx >= x + CW || my < top || my >= bottom) continue;
        if (!tabn[col]) {
            try_drop_tab(col);
            return;
        }
        int r = (my - top) / 16;
        if (r >= tabn[col]) r = tabn[col] - 1;
        if (r < tabup[col]) return;
        if (sel == col && r == sel_i && e->dbl) { auto_found(); return; }
        if (sel >= 0 || sel == -2) {
            int prev = sel;
            int pi = sel_i;
            sel = col;
            sel_i = r;
            if (!try_drop_tab(col)) {
                /* try_drop uses sel as source; restore click as new selection */
                sel = prev;
                sel_i = pi;
                if (!try_drop_tab(col)) {
                    sel = col;
                    sel_i = r;
                }
            }
            return;
        }
        sel = col;
        sel_i = r;
        return;
    }
    sel = -1;
}

void app_sol_open(void) {
    if (!win) new_game();
    if (win) { gui_restore(win); return; }
    int ww = 620, wh = 460;
    if (ww > gfx_width() - 8) ww = gfx_width() - 8;
    if (wh > gfx_height() - 8) wh = gfx_height() - 8;
    win = gui_create("Solitaire", ww, wh, ICON_SOL, sol_event, &win);
    serial_write("APP: SOLITAIRE\n");
}
