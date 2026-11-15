#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "sound.h"
#include "fs.h"
#include "io.h"
#include "serial.h"

static Window *win;
static int tune;

static void player_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int len = sound_length();
    int pos = sound_position();
    if (len < 1) len = 1;
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, sound_tune_name(tune), COL_BLACK, COL_GRAY);
        gfx_sunken(cx + 12, cy + 36, cw - 24, 18, COL_WHITE);
        int bar = (cw - 28) * pos / len;
        if (bar > 0) gfx_fill(cx + 14, cy + 38, bar, 14, COL_NAVY);
        gfx_button(cx + 12, cy + 68, 70, 26, "Play", 0);
        gfx_button(cx + 90, cy + 68, 70, 26, sound_paused() ? "Resume" : "Pause", 0);
        gfx_button(cx + 168, cy + 68, 70, 26, "Stop", 0);
        gfx_text(cx + 12, cy + 110, "Tunes", COL_BLACK, COL_GRAY);
        for (int i = 0; i < sound_tune_count(); i++)
            gfx_button(cx + 12, cy + 132 + i * 28, cw - 24, 24, sound_tune_name(i), tune == i);
        return;
    }
    if (e->type == EVT_DOWN) {
        if (e->y >= 68 && e->y < 94) {
            if (e->x >= 12 && e->x < 82) sound_play_id(tune);
            else if (e->x >= 90 && e->x < 160) sound_pause(!sound_paused());
            else if (e->x >= 168 && e->x < 238) sound_stop();
        }
        int i = (e->y - 132) / 28;
        if (i >= 0 && i < sound_tune_count() && e->x >= 12) {
            tune = i;
            sound_play_id(tune);
        }
        return;
    }
    if (e->type == EVT_KEY && e->code >= '1' && e->code < '1' + sound_tune_count()) {
        tune = e->code - '1';
        sound_play_id(tune);
    }
}

void app_player_open(void) {
    if (win) { gui_restore(win); return; }
    win = gui_create("Media Player", 360, 320, ICON_NOTE, player_event, &win);
    serial_write("APP: PLAYER\n");
}
