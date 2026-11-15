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
static int wallpaper = 0;
static int icon_x[5] = {20, 20, 20, 20, 20};
static int icon_y[5] = {24, 100, 176, 252, 328};
static int safe_mode;

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

static int has_field(const char *text, const char *key) {
    int klen = (int)strlen(key);
    const char *p = text;
    while (*p) {
        if (strncmp(p, key, (size_t)klen) == 0 && p[klen] == '=') return 1;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
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
    if (id < 0) {
        int legacy = fs_lookup(FS_ROOT, "WALL.CFG");
        char old_wall[8];
        uint32_t size = 0;
        if (legacy >= 0 && !fs_read(legacy, old_wall, sizeof(old_wall) - 1, &size) &&
            size && old_wall[0] >= '0' && old_wall[0] <= '3')
            wallpaper = old_wall[0] - '0';
        return;
    }
    char buf[512];
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
    int migrated_wallpaper = !has_field(buf, "wallpaper");
    wallpaper = clamp(field(buf, "wallpaper", wallpaper), 0, 3);
    if (migrated_wallpaper) {
        int legacy = fs_lookup(FS_ROOT, "WALL.CFG");
        char old_wall[8];
        uint32_t size = 0;
        if (legacy >= 0 && !fs_read(legacy, old_wall, sizeof(old_wall) - 1, &size) &&
            size && old_wall[0] >= '0' && old_wall[0] <= '3')
            wallpaper = old_wall[0] - '0';
    }
    for (int i = 0; i < 5; i++) {
        char key[8] = {'i', 'c', 'o', 'n', (char)('0' + i), 'x', 0, 0};
        icon_x[i] = clamp(field(buf, key, icon_x[i]), 0, 10000);
        key[5] = 'y';
        icon_y[i] = clamp(field(buf, key, icon_y[i]), 0, 10000);
    }
    serial_write("SET: scheme ");
    serial_dec((uint32_t)scheme);
    serial_write(" sound ");
    serial_dec((uint32_t)sound_on);
    serial_write(" vol ");
    serial_dec((uint32_t)volume);
    serial_write("\n");
    if (migrated_wallpaper) settings_save();
}

int settings_save(void) {
    char old[512];
    uint32_t old_size = 0;
    int old_id = fs_lookup(FS_ROOT, "SETTINGS.CFG");
    if (old_id >= 0) {
        if (fs_read(old_id, old, sizeof(old), &old_size)) {
            serial_write("SET: backup read failed\n");
            return -1;
        }
        if (fs_write(FS_ROOT, "SETTINGS.BAK", old, old_size) < 0) {
            serial_write("SET: backup write failed\n");
            return -1;
        }
    }
    char buf[512];
    int n = 0;
    const char *keys[] = {"scheme", "saveron", "saver", "mspeed", "mdbl", "mswap",
                          "sound", "volume", "timezone", "wallpaper"};
    int vals[] = {scheme, saver_on, saver_sec, mouse_speed, dbl_ticks, swap_buttons,
                  sound_on, volume, timezone_minutes, wallpaper};
    for (int i = 0; i < 10; i++) {
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
    for (int i = 0; i < 5; i++) {
        for (int axis = 0; axis < 2; axis++) {
            buf[n++] = 'i'; buf[n++] = 'c'; buf[n++] = 'o'; buf[n++] = 'n';
            buf[n++] = (char)('0' + i); buf[n++] = axis ? 'y' : 'x'; buf[n++] = '=';
            int v = axis ? icon_y[i] : icon_x[i];
            char rev[8]; int r = 0;
            if (!v) rev[r++] = '0';
            while (v && r < 8) { rev[r++] = (char)('0' + v % 10); v /= 10; }
            while (r) buf[n++] = rev[--r];
            buf[n++] = '\n';
        }
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

int settings_wallpaper(void) { return wallpaper; }
void settings_set_wallpaper(int id) {
    wallpaper = clamp(id, 0, 3);
    settings_save();
}
int settings_icon_x(int icon) {
    return icon >= 0 && icon < 5 ? icon_x[icon] : 0;
}
int settings_icon_y(int icon) {
    return icon >= 0 && icon < 5 ? icon_y[icon] : 0;
}
void settings_set_icon_position(int icon, int x, int y) {
    if (icon < 0 || icon >= 5) return;
    icon_x[icon] = clamp(x, 0, 10000);
    icon_y[icon] = clamp(y, 0, 10000);
    settings_save();
}
void settings_set_icon_positions(const int x[5], const int y[5]) {
    if (!x || !y) return;
    for (int i = 0; i < 5; i++) {
        icon_x[i] = clamp(x[i], 0, 10000);
        icon_y[i] = clamp(y[i], 0, 10000);
    }
    settings_save();
}
void settings_enter_safe_mode(void) {
    safe_mode = 1;
    scheme = 0;
    saver_on = 0;
    mouse_speed = 2;
    dbl_ticks = 45;
    swap_buttons = 0;
    sound_on = 0;
    volume = 0;
    wallpaper = 0;
    for (int i = 0; i < 5; i++) {
        icon_x[i] = 20;
        icon_y[i] = 24 + i * 76;
    }
}
void settings_reset_defaults(void) {
    safe_mode = 0;
    scheme = 0;
    saver_on = 1;
    saver_sec = 45;
    mouse_speed = 2;
    dbl_ticks = 45;
    swap_buttons = 0;
    sound_on = 1;
    volume = 8;
    timezone_minutes = 0;
    wallpaper = 0;
    for (int i = 0; i < 5; i++) {
        icon_x[i] = 20;
        icon_y[i] = 24 + i * 76;
    }
}
int settings_safe_mode_active(void) { return safe_mode; }
int settings_restore_backup(void) {
    int id = fs_lookup(FS_ROOT, "SETTINGS.BAK");
    if (id < 0) return -1;
    char buf[512];
    uint32_t n = 0;
    if (fs_read(id, buf, sizeof(buf), &n) ||
        fs_write(FS_ROOT, "SETTINGS.CFG", buf, n) < 0 ||
        ata_flush()) {
        serial_write("SET: restore failed\n");
        return -1;
    }
    settings_reset_defaults();
    settings_load();
    return 0;
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
