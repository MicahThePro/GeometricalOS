#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "settings.h"
#include "wallpaper.h"
#include "rtc.h"
#include "net.h"
#include "io.h"
#include "serial.h"

static Window *panel;
static Window *page;
static int which;

static const char *icons[] = {"Color", "Desktop", "Mouse", "Date/Time", "Sound", "Network"};

static void open_page(int id);

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
            gfx_raised(cx + x, cy + y, 36, 36, COL_WHITE);
            gfx_fill(cx + x + 6, cy + y + 6, 24, 24, settings_title_color());
            gfx_text(cx + x + 4, cy + y + 42, icons[i], COL_BLACK, COL_GRAY);
        }
        return;
    }
    if (e->type == EVT_DOWN) {
        for (int i = 0; i < 6; i++) {
            int col = i % 3, row = i / 3;
            int x = 24 + col * 140, y = 40 + row * 90;
            if (e->x >= x && e->x < x + 110 && e->y >= y && e->y < y + 70) open_page(i);
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
            gfx_text(cx + 16, cy + 160, settings_saver_on() ? "Screensaver: on" : "Screensaver: off", COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 182, 80, 24, "Toggle", 0);
            gfx_button(cx + 104, cy + 182, 70, 24, "Shorter", 0);
            gfx_button(cx + 180, cy + 182, 70, 24, "Longer", 0);
        } else if (which == 2) {
            gfx_text(cx + 16, cy + 40, "Pointer speed", COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 64, 70, 24, "Slower", 0);
            gfx_button(cx + 96, cy + 64, 70, 24, "Faster", 0);
            gfx_text(cx + 16, cy + 104, "Double-click", COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 128, 70, 24, "Faster", 0);
            gfx_button(cx + 96, cy + 128, 70, 24, "Slower", 0);
            gfx_button(cx + 16, cy + 170, cw - 32, 26, settings_swap_buttons() ? "Buttons: swapped" : "Buttons: normal", 0);
        } else if (which == 3) {
            RtcTime t;
            rtc_read(&t);
            char b[40];
            b[0] = (char)('0' + (t.month / 10) % 10); b[1] = (char)('0' + t.month % 10); b[2] = '/';
            b[3] = (char)('0' + (t.day / 10) % 10); b[4] = (char)('0' + t.day % 10); b[5] = '/';
            b[6] = (char)('0' + ((t.year / 1000) % 10)); b[7] = (char)('0' + ((t.year / 100) % 10));
            b[8] = (char)('0' + ((t.year / 10) % 10)); b[9] = (char)('0' + (t.year % 10));
            b[10] = ' ';
            b[11] = (char)('0' + (t.hour / 10) % 10); b[12] = (char)('0' + t.hour % 10); b[13] = ':';
            b[14] = (char)('0' + (t.min / 10) % 10); b[15] = (char)('0' + t.min % 10); b[16] = 0;
            gfx_text(cx + 16, cy + 48, b, COL_BLACK, COL_GRAY);
            gfx_text(cx + 16, cy + 80, "The clock follows the virtual machine.", COL_BLACK, COL_GRAY);
        } else if (which == 4) {
            gfx_button(cx + 16, cy + 40, cw - 32, 26, settings_sound_on() ? "Sounds: on" : "Sounds: off", 0);
            gfx_text(cx + 16, cy + 80, "Volume", COL_BLACK, COL_GRAY);
            gfx_button(cx + 16, cy + 104, 70, 24, "Quieter", 0);
            gfx_button(cx + 96, cy + 104, 70, 24, "Louder", 0);
        } else if (which == 5) {
            if (!net_ready()) gfx_text(cx + 16, cy + 40, "Network is not configured yet.", COL_BLACK, COL_GRAY);
            else {
                char line[40];
                strcpy(line, "IP ");
                net_format_ip(net_ip(), line + 3, 20);
                gfx_text(cx + 16, cy + 36, line, COL_BLACK, COL_GRAY);
                strcpy(line, "Mask ");
                net_format_ip(net_mask(), line + 5, 20);
                gfx_text(cx + 16, cy + 56, line, COL_BLACK, COL_GRAY);
                strcpy(line, "Gateway ");
                net_format_ip(net_gateway(), line + 8, 20);
                gfx_text(cx + 16, cy + 76, line, COL_BLACK, COL_GRAY);
                strcpy(line, "DNS ");
                net_format_ip(net_dns(), line + 4, 20);
                gfx_text(cx + 16, cy + 96, line, COL_BLACK, COL_GRAY);
            }
            gfx_button(cx + 16, cy + 130, 140, 26, "Renew DHCP", 0);
            gfx_text(cx + 16, cy + 170, "Use NAT and PCnet-FAST III.", COL_BLACK, COL_GRAY);
        }
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
        if (e->y >= 182 && e->y < 206) {
            if (e->x >= 16 && e->x < 96) settings_set_saver(!settings_saver_on(), settings_saver_seconds());
            else if (e->x >= 104 && e->x < 174) settings_set_saver(settings_saver_on(), settings_saver_seconds() - 10);
            else if (e->x >= 180 && e->x < 250) settings_set_saver(settings_saver_on(), settings_saver_seconds() + 15);
            gui_invalidate();
        }
    } else if (which == 2) {
        if (e->y >= 64 && e->y < 88) {
            if (e->x >= 16 && e->x < 86) settings_set_mouse(settings_mouse_speed() - 1, settings_dbl_ticks(), settings_swap_buttons());
            else if (e->x >= 96 && e->x < 166) settings_set_mouse(settings_mouse_speed() + 1, settings_dbl_ticks(), settings_swap_buttons());
        } else if (e->y >= 128 && e->y < 152) {
            if (e->x >= 16 && e->x < 86) settings_set_mouse(settings_mouse_speed(), settings_dbl_ticks() - 10, settings_swap_buttons());
            else if (e->x >= 96 && e->x < 166) settings_set_mouse(settings_mouse_speed(), settings_dbl_ticks() + 10, settings_swap_buttons());
        } else if (e->y >= 170 && e->y < 196) {
            settings_set_mouse(settings_mouse_speed(), settings_dbl_ticks(), !settings_swap_buttons());
        }
        gui_invalidate();
    } else if (which == 4) {
        if (e->y >= 40 && e->y < 66) settings_set_sound(!settings_sound_on(), settings_volume());
        else if (e->y >= 104 && e->y < 128) {
            if (e->x >= 16 && e->x < 86) settings_set_sound(settings_sound_on(), settings_volume() - 2);
            else if (e->x >= 96 && e->x < 166) settings_set_sound(settings_sound_on(), settings_volume() + 2);
        }
        gui_invalidate();
    } else if (which == 5 && e->y >= 130 && e->y < 156 && e->x >= 16 && e->x < 156) {
        net_init();
        gui_invalidate();
    }
}

static void open_page(int id) {
    which = id;
    if (page) { gui_restore(page); gui_focus(page); return; }
    page = gui_create(icons[id], 360, 280, ICON_GROUP, page_event, &page);
}

void app_control_open(void) {
    if (panel) { gui_restore(panel); return; }
    panel = gui_create("Control Panel", 460, 260, ICON_GROUP, panel_event, &panel);
    serial_write("APP: CONTROL\n");
}
