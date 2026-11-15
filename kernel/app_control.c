#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "settings.h"
#include "wallpaper.h"
#include "rtc.h"
#include "sound.h"
#include "io.h"
#include "serial.h"

static Window *panel;
static Window *page;
static int which;
static int panel_selected;
static int drag_slider = -1;
static char sound_msg[64];

static const char *icons[] = {"Color", "Desktop", "Mouse", "Date/Time", "Sound", "System"};

static void open_page(int id);

static void put2(char *out, int value) {
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
}

static void put4(char *out, int value) {
    out[0] = (char)('0' + (value / 1000) % 10);
    out[1] = (char)('0' + (value / 100) % 10);
    out[2] = (char)('0' + (value / 10) % 10);
    out[3] = (char)('0' + value % 10);
}

static int parse_digits(const char *text, int length, int *value) {
    if ((int)strlen(text) != length) return 0;
    int result = 0;
    for (int i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        result = result * 10 + text[i] - '0';
    }
    *value = result;
    return 1;
}

static int parse_timezone(const char *text, int *minutes) {
    int sign = 1, value = 0, digits = 0;
    if (*text == '-') { sign = -1; text++; }
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + *text++ - '0';
        if (value > 840) return 0;
        digits++;
    }
    if (!digits || *text || value % 15) return 0;
    value *= sign;
    if (value < -720 || value > 840) return 0;
    *minutes = value;
    return 1;
}

static void format_timezone(char *out, int minutes) {
    int magnitude = minutes < 0 ? -minutes : minutes;
    out[0] = 'U'; out[1] = 'T'; out[2] = 'C';
    out[3] = minutes < 0 ? '-' : '+';
    put2(out + 4, magnitude / 60);
    out[6] = ':';
    put2(out + 7, magnitude % 60);
    out[9] = 0;
}

static void format_offset_input(char *out, int minutes) {
    int value = minutes < 0 ? -minutes : minutes;
    int n = 0;
    if (minutes < 0) out[n++] = '-';
    if (value >= 100) out[n++] = (char)('0' + value / 100);
    if (value >= 10) out[n++] = (char)('0' + (value / 10) % 10);
    out[n++] = (char)('0' + value % 10);
    out[n] = 0;
}

static void panel_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, "Control Panel", COL_BLACK, COL_GRAY);
        for (int i = 0; i < 6; i++) {
            int col = i % 3;
            int row = i / 3;
            int x = 24 + col * 140;
            int y = 40 + row * 90;
            gfx_button(cx + x, cy + y, 110, 70, 0, i == panel_selected);
            gfx_raised(cx + x + 36, cy + y + 4, 36, 36, COL_WHITE);
            gfx_fill(cx + x + 6, cy + y + 6, 24, 24, settings_title_color());
            gfx_text(cx + x + (110 - gfx_text_width(icons[i])) / 2,
                     cy + y + 48, icons[i], COL_BLACK, COL_GRAY);
        }
        return;
    }
    if (e->type == EVT_KEY) {
        if (e->code >= '1' && e->code <= '6') {
            panel_selected = e->code - '1';
            open_page(panel_selected);
        } else if (e->code == KEY_RIGHT) panel_selected = (panel_selected + 1) % 6;
        else if (e->code == KEY_LEFT) panel_selected = (panel_selected + 5) % 6;
        else if (e->code == KEY_DOWN) panel_selected = (panel_selected + 3) % 6;
        else if (e->code == KEY_UP) panel_selected = (panel_selected + 3) % 6;
        else if (e->code == 10 || e->code == 13) open_page(panel_selected);
        else return;
        gui_invalidate();
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < 6; i++) {
            int col = i % 3, row = i / 3;
            int x = 24 + col * 140, y = 40 + row * 90;
            if (e->x >= x && e->x < x + 110 && e->y >= y && e->y < y + 70) {
                panel_selected = i;
                open_page(i);
            }
        }
    }
}

static void page_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 12, cy + 8, icons[which], COL_BLACK, COL_GRAY);
        if (which == 0) {
            static const char *names[] = {"Teal", "Blue", "Green", "Slate", "Rose"};
            for (int i = 0; i < 5; i++)
                gfx_button(cx + 16, cy + 36 + i * 32, cw - 32, 26, names[i], settings_scheme() == i);
        } else if (which == 1) {
            static const char *names[] = {"Solid", "Tiles", "Stripes", "Diamonds"};
            for (int i = 0; i < 4; i++)
                gfx_button(cx + 16, cy + 36 + i * 28, 140, 24, names[i], wall_get() == i);
            gfx_button(cx + 16, cy + 156, 100, 24, settings_saver_on() ? "Saver: on" : "Saver: off", 0);
            gfx_text(cx + 130, cy + 160, "Wait", COL_BLACK, COL_GRAY);
            gfx_slider(cx + 16, cy + 188, cw - 32, settings_saver_seconds(), 5, 300, drag_slider == 0);
            gfx_text(cx + 16, cy + 214, "5s", COL_BLACK, COL_GRAY);
            gfx_text(cx + cw - 40, cy + 214, "5 min", COL_BLACK, COL_GRAY);
        } else if (which == 2) {
            gfx_text(cx + 16, cy + 36, "Slow", COL_BLACK, COL_GRAY);
            gfx_slider(cx + 16, cy + 52, cw - 32, settings_mouse_speed(), 1, 4, drag_slider == 0);
            gfx_text(cx + cw - 48, cy + 36, "Fast", COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 88, "Double-click", COL_BLACK, COL_GRAY);
            gfx_slider(cx + 16, cy + 108, cw - 32, settings_dbl_ticks(), 15, 90, drag_slider == 1);
            gfx_button(cx + 16, cy + 150, cw - 32, 26, settings_swap_buttons() ? "Buttons: swapped" : "Buttons: normal", 0);
        } else if (which == 3) {
            RtcTime t;
            rtc_read(&t);
            char date[11], time[9], zone[10];
            put4(date, t.year); date[4] = '-'; put2(date + 5, t.month);
            date[7] = '-'; put2(date + 8, t.day); date[10] = 0;
            put2(time, t.hour); time[2] = ':'; put2(time + 3, t.min);
            time[5] = ':'; put2(time + 6, t.sec); time[8] = 0;
            format_timezone(zone, settings_timezone_minutes());
            gfx_text(cx + 16, cy + 38, "Local date:", COL_BLACK, COL_GRAY);
            gfx_text(cx + 112, cy + 38, date, COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 58, "Local time:", COL_BLACK, COL_GRAY);
            gfx_text(cx + 112, cy + 58, time, COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 78, "Time zone:", COL_BLACK, COL_GRAY);
            gfx_text(cx + 112, cy + 78, zone, COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 112, 145, 26, "Set Date", 0);
            gfx_button(cx + 176, cy + 112, 145, 26, "Set Time", 0);
            gfx_button(cx + 16, cy + 150, cw - 32, 26, "Set UTC Offset", 0);
            gfx_text(cx + 16, cy + 188, "RTC stores UTC; fixed offset, no DST.", COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 208, "Offsets use minutes; 345 = UTC+05:45.", COL_BLACK, COL_GRAY);
        } else if (which == 4) {
            gfx_button(cx + 16, cy + 36, 120, 26, settings_sound_on() ? "Sounds: on" : "Sounds: off", 0);
            gfx_button(cx + 148, cy + 36, 80, 26, "Test", 0);
            gfx_text(cx + 16, cy + 74, "Quiet", COL_BLACK, COL_GRAY);
            gfx_slider(cx + 16, cy + 90, cw - 32, settings_volume(), 0, 15, drag_slider == 0);
            gfx_text(cx + cw - 48, cy + 74, "Loud", COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 130, sound_msg[0] ? sound_msg : sound_detect_text(), COL_BLACK, COL_GRAY);
        } else if (which == 5) {
            gfx_text(cx + 16, cy + 36, "Recovery tools", COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 60, cw - 32, 26, "Restore Last Good Settings", 0);
            gfx_button(cx + 16, cy + 94, cw - 32, 26, "Reset All Settings to Defaults", 0);
            gfx_text(cx + 16, cy + 138, "Settings are backed up before each save.", COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 158, "F8 at startup opens Recovery.", COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 186, sound_msg, COL_NAVY, COL_GRAY);
        }
        return;
    }
    if (e->type == EVT_UP) { drag_slider = -1; return; }
    if (e->type == EVT_KEY && which == 5 &&
        (e->code == 'r' || e->code == 'R')) {
        if (settings_restore_backup() == 0) {
            wall_load();
            strncpy(sound_msg, "Restored last good settings.", sizeof(sound_msg));
        } else {
            strncpy(sound_msg, "No usable backup was found.", sizeof(sound_msg));
        }
        return;
    }
    if (e->type == EVT_KEY && which == 5 &&
        (e->code == 'd' || e->code == 'D')) {
        settings_reset_defaults();
        if (settings_save() == 0) {
            wall_load();
            strncpy(sound_msg, "Default settings saved.", sizeof(sound_msg));
        } else {
            strncpy(sound_msg, "Could not save defaults.", sizeof(sound_msg));
        }
        return;
    }
    if (e->type == EVT_KEY && which == 0 && (e->code == 'b' || e->code == 'B')) {
        settings_set_scheme(1);
        return;
    }
    if (e->type == EVT_KEY && drag_slider < 0) {
        int dir = 0;
        if (e->code == KEY_LEFT) dir = -1;
        else if (e->code == KEY_RIGHT) dir = 1;
        else if (e->code == KEY_HOME) dir = -100;
        else if (e->code == KEY_END) dir = 100;
        if (!dir) return;
        if (which == 1) settings_set_saver(settings_saver_on(), settings_saver_seconds() + (dir < 0 ? -15 : 15));
        else if (which == 2) settings_set_mouse(settings_mouse_speed() + (dir < 0 ? -1 : 1), settings_dbl_ticks(), settings_swap_buttons());
        else if (which == 4) settings_set_sound(settings_sound_on(), settings_volume() + dir);
        return;
    }
    if (e->type == EVT_MOVE && drag_slider >= 0) {
        int val = gfx_slider_value(e->x + 0, 16, cw - 32, 0, 1);
        (void)val;
        if (which == 1) settings_set_saver(settings_saver_on(), gfx_slider_value(e->x, 16, cw - 32, 5, 300));
        else if (which == 2 && drag_slider == 0) settings_set_mouse(gfx_slider_value(e->x, 16, cw - 32, 1, 4), settings_dbl_ticks(), settings_swap_buttons());
        else if (which == 2 && drag_slider == 1) settings_set_mouse(settings_mouse_speed(), gfx_slider_value(e->x, 16, cw - 32, 15, 90), settings_swap_buttons());
        else if (which == 4) settings_set_sound(settings_sound_on(), gfx_slider_value(e->x, 16, cw - 32, 0, 15));
        return;
    }
    if (e->type != EVT_DOWN) return;
    if (which == 0) {
        int i = (e->y - 36) / 32;
        if (i >= 0 && i < 5 && e->x >= 16) { settings_set_scheme(i); gui_invalidate(); }
    } else if (which == 1) {
        int i = (e->y - 36) / 28;
        if (i >= 0 && i < 4 && e->y < 36 + 4 * 28 && e->x >= 16 && e->x < 160) {
            wall_set(i); gui_invalidate();
        }
        if (e->y >= 156 && e->y < 180 && e->x >= 16 && e->x < 116)
            settings_set_saver(!settings_saver_on(), settings_saver_seconds());
        if (e->y >= 188 && e->y < 214) {
            drag_slider = 0;
            settings_set_saver(settings_saver_on(), gfx_slider_value(e->x, 16, cw - 32, 5, 300));
        }
        gui_invalidate();
    } else if (which == 2) {
        if (e->y >= 52 && e->y < 76) {
            drag_slider = 0;
            settings_set_mouse(gfx_slider_value(e->x, 16, cw - 32, 1, 4), settings_dbl_ticks(), settings_swap_buttons());
        } else if (e->y >= 108 && e->y < 132) {
            drag_slider = 1;
            settings_set_mouse(settings_mouse_speed(), gfx_slider_value(e->x, 16, cw - 32, 15, 90), settings_swap_buttons());
        } else if (e->y >= 150 && e->y < 176) {
            settings_set_mouse(settings_mouse_speed(), settings_dbl_ticks(), !settings_swap_buttons());
        }
        gui_invalidate();
    } else if (which == 3) {
        RtcTime t;
        rtc_read(&t);
        if (e->y >= 112 && e->y < 138 && e->x >= 16 && e->x < 161) {
            char value[9];
            put4(value, t.year); put2(value + 4, t.month); put2(value + 6, t.day); value[8] = 0;
            if (gui_prompt("Set Date", "Date as YYYYMMDD (2000-2099):", value, sizeof(value))) {
                int date;
                if (!parse_digits(value, 8, &date)) {
                    gui_dialog("Set Date", "Enter exactly eight digits: YYYYMMDD.", "OK", 0, 0);
                } else {
                    t.year = date / 10000;
                    t.month = (date / 100) % 100;
                    t.day = date % 100;
                    if (rtc_set_local(&t) < 0)
                        gui_dialog("Set Date", "Invalid date or unsupported RTC year.", "OK", 0, 0);
                }
            }
        } else if (e->y >= 112 && e->y < 138 && e->x >= 176 && e->x < 321) {
            char value[7];
            put2(value, t.hour); put2(value + 2, t.min); put2(value + 4, t.sec); value[6] = 0;
            if (gui_prompt("Set Time", "Time as HHMMSS (24-hour):", value, sizeof(value))) {
                int time;
                if (!parse_digits(value, 6, &time)) {
                    gui_dialog("Set Time", "Enter exactly six digits: HHMMSS.", "OK", 0, 0);
                } else {
                    t.hour = time / 10000;
                    t.min = (time / 100) % 100;
                    t.sec = time % 100;
                    if (rtc_set_local(&t) < 0)
                        gui_dialog("Set Time", "Invalid time or unsupported RTC year.", "OK", 0, 0);
                }
            }
        } else if (e->y >= 150 && e->y < 176 && e->x >= 16 && e->x < cw - 16) {
            char value[8];
            format_offset_input(value, settings_timezone_minutes());
            if (gui_prompt("Set UTC Offset", "Minutes from UTC (-720..840, step 15):", value, sizeof(value))) {
                int offset;
                if (!parse_timezone(value, &offset)) {
                    gui_dialog("Set UTC Offset", "Use -720 to 840 minutes in steps of 15.", "OK", 0, 0);
                } else {
                    if (settings_set_timezone_minutes(offset) < 0)
                        gui_dialog("Set UTC Offset", "Could not save the setting to disk.", "OK", 0, 0);
                }
            }
        }
        gui_invalidate();
    } else if (which == 4) {
        if (e->y >= 36 && e->y < 62 && e->x >= 16 && e->x < 136)
            settings_set_sound(!settings_sound_on(), settings_volume());
        else if (e->y >= 36 && e->y < 62 && e->x >= 148 && e->x < 228) {
            int result = sound_test();
            if (result < 0)
                strncpy(sound_msg, "SB16 detected, but playback failed.", sizeof(sound_msg));
            else
                strncpy(sound_msg, sound_detect_text(), sizeof(sound_msg));
        } else if (e->y >= 90 && e->y < 114) {
            drag_slider = 0;
            settings_set_sound(settings_sound_on(), gfx_slider_value(e->x, 16, cw - 32, 0, 15));
        }
        gui_invalidate();
    } else if (which == 5) {
        if (e->y >= 60 && e->y < 86 && e->x >= 16) {
            if (settings_restore_backup() == 0) {
                wall_load();
                strncpy(sound_msg, "Restored last good settings.", sizeof(sound_msg));
            } else {
                strncpy(sound_msg, "No usable backup was found.", sizeof(sound_msg));
            }
        } else if (e->y >= 94 && e->y < 120 && e->x >= 16) {
            settings_reset_defaults();
            if (settings_save() == 0) {
                wall_load();
                strncpy(sound_msg, "Default settings saved.", sizeof(sound_msg));
            } else {
                strncpy(sound_msg, "Could not save defaults.", sizeof(sound_msg));
            }
        }
        gui_invalidate();
    }
}

static void open_page(int id) {
    which = id;
    if (page) {
        strncpy(page->title, icons[id], sizeof(page->title));
        gui_restore(page);
        gui_focus(page);
        return;
    }
    page = gui_create(icons[id], 360, 280, ICON_GROUP, page_event, &page);
}

void app_control_open(void) {
    if (panel) { gui_restore(panel); return; }
    panel = gui_create("Control Panel", 460, 260, ICON_GROUP, panel_event, &panel);
    serial_write("APP: CONTROL\n");
}
