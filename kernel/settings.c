#include "settings.h"
#include "fs.h"
#include "io.h"
#include "ata.h"
#include "serial.h"

static int scheme = 0;
static int saver_on = 1;
static int saver_sec = 45;
static int mouse_speed = 2;
static int dbl_ticks = 45;
static int swap_buttons = 0;
static int sound_on = 1;
static int volume = 8;
static int timezone_minutes = 0;

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int field(const char *text, const char *key, int fallback) {
    int klen = (int)strlen(key);
    const char *p = text;
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=') {
            int v = 0;
            const char *n = p + klen + 1;
            if (*n < '0' || *n > '9') return fallback;
            while (*n >= '0' && *n <= '9') v = v * 10 + (*n++ - '0');
            return v;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return fallback;
}

static int signed_field(const char *text, const char *key, int fallback) {
    int klen = (int)strlen(key);
    const char *p = text;
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=') {
            const char *n = p + klen + 1;
            int sign = 1, v = 0, digits = 0;
            if (*n == '-') { sign = -1; n++; }
            while (*n >= '0' && *n <= '9') {
                if (v > 10000) return fallback;
                v = v * 10 + (*n++ - '0');
                digits++;
            }
            return digits ? sign * v : fallback;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return fallback;
}

void settings_load(void) {
    int id = fs_lookup(FS_ROOT, "SETTINGS.CFG");
    if (id < 0) return;
    char buf[256];
    uint32_t n = 0;
    memset(buf, 0, sizeof(buf));
    if (fs_read(id, buf, sizeof(buf) - 1, &n)) return;
    scheme = clamp(field(buf, "scheme", scheme), 0, 4);
    saver_on = field(buf, "saveron", saver_on) ? 1 : 0;
    saver_sec = clamp(field(buf, "saver", saver_sec), 5, 300);
    mouse_speed = clamp(field(buf, "mspeed", mouse_speed), 1, 4);
    dbl_ticks = clamp(field(buf, "mdbl", dbl_ticks), 15, 90);
    swap_buttons = field(buf, "mswap", swap_buttons) ? 1 : 0;
    sound_on = field(buf, "sound", sound_on) ? 1 : 0;
    volume = clamp(field(buf, "volume", volume), 0, 15);
    timezone_minutes = clamp(signed_field(buf, "timezone", timezone_minutes), -720, 840);
    if (timezone_minutes % 15) timezone_minutes = 0;
    serial_write("SET: scheme ");
    serial_dec((uint32_t)scheme);
    serial_write(" sound ");
    serial_dec((uint32_t)sound_on);
    serial_write(" vol ");
    serial_dec((uint32_t)volume);
    serial_write("\n");
}

int settings_save(void) {
    char buf[160];
    int n = 0;
    const char *keys[] = {"scheme", "saveron", "saver", "mspeed", "mdbl", "mswap", "sound", "volume", "timezone"};
    int vals[] = {scheme, saver_on, saver_sec, mouse_speed, dbl_ticks, swap_buttons, sound_on, volume, timezone_minutes};
    for (int i = 0; i < 9; i++) {
        const char *k = keys[i];
        while (*k && n < 150) buf[n++] = *k++;
        if (n < 150) buf[n++] = '=';
        char rev[8];
        int r = 0, v = vals[i];
        int negative = v < 0;
        if (negative) v = -v;
        if (!v) rev[r++] = '0';
        while (v && r < 8) { rev[r++] = (char)('0' + v % 10); v /= 10; }
        if (negative && n < 150) buf[n++] = '-';
        while (r && n < 150) buf[n++] = rev[--r];
        if (n < 150) buf[n++] = '\n';
    }
    buf[n] = 0;
    if (fs_write(FS_ROOT, "SETTINGS.CFG", buf, (uint32_t)n) < 0) {
        serial_write("SET: write failed\n");
        return -1;
    }
    if (ata_flush()) {
        serial_write("SET: flush failed\n");
        return -1;
    }
    serial_write("SET: saved\n");
    return 0;
}

int settings_scheme(void) { return scheme; }
void settings_set_scheme(int s) { scheme = clamp(s, 0, 4); settings_save(); }

uint32_t settings_desktop_color(void) {
    static const uint32_t c[] = {0x008080u, 0x000080u, 0x004020u, 0x202020u, 0x603040u};
    return c[scheme];
}
uint32_t settings_title_color(void) {
    static const uint32_t c[] = {0x000080u, 0x000060u, 0x004000u, 0x404040u, 0x800040u};
    return c[scheme];
}
uint32_t settings_highlight_color(void) {
    return settings_title_color();
}

int settings_saver_on(void) { return saver_on; }
int settings_saver_seconds(void) { return saver_sec; }
void settings_set_saver(int on, int seconds) {
    saver_on = on ? 1 : 0;
    saver_sec = clamp(seconds, 5, 300);
    settings_save();
}

int settings_mouse_speed(void) { return mouse_speed; }
int settings_dbl_ticks(void) { return dbl_ticks; }
int settings_swap_buttons(void) { return swap_buttons; }
void settings_set_mouse(int speed, int dbl, int swap) {
    mouse_speed = clamp(speed, 1, 4);
    dbl_ticks = clamp(dbl, 15, 90);
    swap_buttons = swap ? 1 : 0;
    settings_save();
}

int settings_sound_on(void) { return sound_on; }
int settings_volume(void) { return volume; }
void settings_set_sound(int on, int vol) {
    sound_on = on ? 1 : 0;
    volume = clamp(vol, 0, 15);
    settings_save();
}

int settings_timezone_minutes(void) { return timezone_minutes; }
int settings_set_timezone_minutes(int minutes) {
    int previous = timezone_minutes;
    timezone_minutes = clamp(minutes, -720, 840);
    if (settings_save() < 0) {
        timezone_minutes = previous;
        return -1;
    }
    return 0;
}
