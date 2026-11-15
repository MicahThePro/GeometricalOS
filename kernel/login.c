#include "apps.h"
#include "gfx.h"
#include "input.h"
#include "fs.h"
#include "rtc.h"
#include "io.h"
#include "serial.h"

static void present(void) {
    gfx_present();
    cursor_invalidate();
    cursor_draw(mouse_x, mouse_y);
}

/* Returns 1 when the user may enter the desktop. */
int login_screen(void) {
    int id = fs_lookup(FS_ROOT, "USER.DAT");
    if (id < 0) return 1;
    char raw[80];
    uint32_t n = 0;
    memset(raw, 0, sizeof(raw));
    if (fs_read(id, raw, sizeof(raw) - 1, &n)) return 1;
    char name[28], pass[28];
    memset(name, 0, sizeof(name));
    memset(pass, 0, sizeof(pass));
    int i = 0, j = 0;
    while (raw[i] && raw[i] != '\n' && j < 27) name[j++] = raw[i++];
    if (raw[i] == '\n') i++;
    j = 0;
    while (raw[i] && raw[i] != '\n' && j < 27) pass[j++] = raw[i++];
    if (!name[0]) return 1;

    char typed_name[28], typed_pass[28];
    strncpy(typed_name, name, 28);
    typed_pass[0] = 0;
    int which = pass[0] ? 1 : 0; /* 0 editing name, 1 editing password */
    int nlen = (int)strlen(typed_name);
    int plen = 0;
    serial_write("LOGIN: READY\n");

    for (;;) {
        gfx_reset_clip();
        gfx_clear(COL_SETUPBLUE);
        int w = 420, h = 230;
        int x = (gfx_width() - w) / 2;
        int y = (gfx_height() - h) / 2;
        gfx_raised(x, y, w, h, COL_GRAY);
        gfx_fill(x + 3, y + 3, w - 6, 18, COL_NAVY);
        gfx_text(x + 8, y + 4, "GeometricalOS Logon", COL_WHITE, COL_NAVY);
        gfx_logo(x + 36, y + 70, 1);
        gfx_text(x + 90, y + 36, "Type your name, then ENTER.", COL_BLACK, COL_GRAY);
        gfx_text(x + 90, y + 70, "User name", COL_BLACK, COL_GRAY);
        gfx_sunken(x + 90, y + 88, 300, 22, which == 0 ? COL_WHITE : COL_GRAY);
        gfx_text(x + 96, y + 92, typed_name, COL_BLACK, which == 0 ? COL_WHITE : COL_GRAY);
        gfx_text(x + 90, y + 120, pass[0] ? "Password" : "Password (none set)", COL_BLACK, COL_GRAY);
        gfx_sunken(x + 90, y + 138, 300, 22, which == 1 ? COL_WHITE : COL_GRAY);
        char stars[28];
        for (int s = 0; s < plen && s < 27; s++) stars[s] = '*';
        stars[plen < 27 ? plen : 27] = 0;
        gfx_text(x + 96, y + 142, stars, COL_BLACK, which == 1 ? COL_WHITE : COL_GRAY);
        gfx_button(x + w - 110, y + h - 36, 90, 24, "Log on", 0);
        present();

        input_poll();
        int clicked = 0;
        MouseEvent m;
        while (input_mouse(&m)) {
            if (m.type == MOUSE_LDOWN) {
                if (m.x >= x + 90 && m.x < x + 390 && m.y >= y + 88 && m.y < y + 110) which = 0;
                if (m.x >= x + 90 && m.x < x + 390 && m.y >= y + 138 && m.y < y + 160) which = 1;
                if (m.x >= x + w - 110 && m.x < x + w - 20 && m.y >= y + h - 36 && m.y < y + h - 12)
                    clicked = 1;
            }
        }
        KeyEvent k;
        int submit = clicked;
        while (input_key(&k)) {
            if (k.code == 10 || k.code == 13) {
                if (which == 0 && pass[0]) which = 1;
                else submit = 1;
            } else if (k.code == 9) which ^= 1;
            else if (k.code == 8) {
                if (which == 0 && nlen > 0) typed_name[--nlen] = 0;
                if (which == 1 && plen > 0) typed_pass[--plen] = 0;
            } else if (k.code >= 32 && k.code < 127) {
                char *dst = which ? typed_pass : typed_name;
                int *len = which ? &plen : &nlen;
                if (*len < 27) {
                    dst[(*len)++] = (char)k.code;
                    dst[*len] = 0;
                }
            }
        }
        if (!submit) {
            cursor_restore();
            cursor_draw(mouse_x, mouse_y);
            continue;
        }
        if (strcmp(typed_name, name) != 0 || (pass[0] && strcmp(typed_pass, pass) != 0)) {
            gfx_text(x + 90, y + 168, "That name or password is not right.", COL_MAROON, COL_GRAY);
            present();
            sleep_seconds(1);
            continue;
        }
        serial_write("LOGIN: OK\n");
        return 1;
    }
}
