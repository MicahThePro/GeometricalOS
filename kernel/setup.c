#include "apps.h"
#include "gfx.h"
#include "input.h"
#include "ata.h"
#include "fs.h"
#include "bootinfo.h"
#include "sound.h"
#include "serial.h"
#include "io.h"
#include "rtc.h"
#include "settings.h"
#include "pci.h"

static char acct_name[28] = "User";
static char acct_pass[28];

extern const unsigned char mbr_template[];
extern const unsigned int mbr_template_len;
extern char kernel_file_end[];

static int dx, dy, dw, dh;
static int busy_pct;
static const char *busy_msg;
static RtcTime install_clock;
static int install_timezone;
static int install_clock_from_rtc;
static int setup_vm_detected;

static int ask_line(const char *marker, const char *heading, const char *prompt,
                    char *buf, int secret);

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

static int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) return 29;
    return month >= 1 && month <= 12 ? days[month - 1] : 0;
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

static int cpuid_available(void) {
    uint32_t original, changed;
    __asm__ volatile ("pushfl\n\tpopl %0" : "=r"(original));
    changed = original ^ (1u << 21);
    __asm__ volatile ("pushl %0\n\tpopfl" : : "r"(changed) : "cc", "memory");
    __asm__ volatile ("pushfl\n\tpopl %0" : "=r"(changed));
    __asm__ volatile ("pushl %0\n\tpopfl" : : "r"(original) : "cc", "memory");
    return ((original ^ changed) & (1u << 21)) != 0;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static int detect_virtual_machine(void) {
    int bus, dev, fn;
    if (pci_find(0x80EE, 0xCAFE, &bus, &dev, &fn) == 0) return 1;
    if (!cpuid_available()) return 0;
    uint32_t max_leaf, b, c, d;
    cpuid(0, &max_leaf, &b, &c, &d);
    if (max_leaf < 1) return 0;
    uint32_t a;
    cpuid(1, &a, &b, &c, &d);
    return (c & 0x80000000u) != 0;
}

static void present(void) {
    gfx_present();
    cursor_invalidate();
    cursor_draw(mouse_x, mouse_y);
}

static void frame(const char *title) {
    gfx_reset_clip();
    gfx_clear(COL_SETUPBLUE);
    dw = gfx_width() >= 720 ? 560 : 480;
    dh = gfx_height() >= 500 ? 340 : 300;
    dx = (gfx_width() - dw) / 2;
    dy = (gfx_height() - dh) / 2;
    gfx_raised(dx, dy, dw, dh, COL_GRAY);
    gfx_fill(dx + 3, dy + 3, dw - 6, 18, COL_NAVY);
    gfx_text(dx + 8, dy + 4, title, COL_WHITE, COL_NAVY);
    gfx_logo(dx + 46, dy + 78, 1);
}

static void line(int y, const char *s, uint32_t color) {
    gfx_text(dx + 96, dy + y, s, color, COL_GRAY);
}

static void button(int x, int y, int w, const char *label) {
    gfx_button(x, y, w, 26, label, 0);
}

static void button_state(int x, int y, int w, const char *label, int pressed) {
    gfx_button(x, y, w, 26, label, pressed);
}

static int point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* Returns 1 for the primary button / Enter, 2 for secondary / Esc. */
static int wait_buttons(const char *primary, const char *secondary) {
    int bw = 110;
    int y = dy + dh - 42;
    int x2 = dx + dw - 16 - bw;
    int x1 = secondary ? x2 - 12 - bw : x2;
    for (;;) {
        frame(busy_msg ? "GeometricalOS Setup" : "GeometricalOS Setup");
        /* frame() clears the dialog, so callers redraw via a callback stored... 
           This helper is only used after the caller has drawn into a fresh frame.
           Redraw is handled by the caller loop instead. */
        (void)primary; (void)secondary; (void)bw; (void)y; (void)x1; (void)x2;
        return 0;
    }
}

static void draw_buttons(const char *primary, const char *secondary) {
    int bw = 120;
    int y = dy + dh - 42;
    int x2 = dx + dw - 18 - bw;
    int x1 = secondary ? x2 - 10 - bw : x2;
    button(x1, y, bw, primary);
    if (secondary) button(x2, y, bw, secondary);
}

static int poll_choice(const char *primary, const char *secondary) {
    static int armed;
    static int armed_choice;
    int bw = 120;
    int y = dy + dh - 42;
    int x2 = dx + dw - 18 - bw;
    int x1 = secondary ? x2 - 10 - bw : x2;
    input_poll();
    MouseEvent m;
    while (input_mouse(&m)) {
        if (m.type == MOUSE_LDOWN) {
            if (point_in(m.x, m.y, x1, y, bw, 26)) armed_choice = 1;
            else if (secondary && point_in(m.x, m.y, x2, y, bw, 26)) armed_choice = 2;
            else continue;
            armed = 1;
            button_state(armed_choice == 1 ? x1 : x2, y, bw,
                         armed_choice == 1 ? primary : secondary, 1);
            present();
        } else if (m.type == MOUSE_LUP && armed) {
            int choice = armed_choice;
            int inside = choice == 1 ? point_in(m.x, m.y, x1, y, bw, 26) :
                         point_in(m.x, m.y, x2, y, bw, 26);
            button_state(choice == 1 ? x1 : x2, y, bw,
                         choice == 1 ? primary : secondary, 0);
            present();
            armed = 0;
            if (inside) return choice;
        }
    }
    KeyEvent k;
    while (input_key(&k)) {
        if (k.code == 10 || k.code == 13 || k.code == 27) {
            int choice = k.code == 27 && secondary ? 2 : 1;
            button_state(choice == 1 ? x1 : x2, y, bw,
                         choice == 1 ? primary : secondary, 1);
            present();
            button_state(choice == 1 ? x1 : x2, y, bw,
                         choice == 1 ? primary : secondary, 0);
            present();
            return choice;
        }
    }
    return 0;
}

static void progress_ui(int pct, const char *msg) {
    busy_pct = pct;
    busy_msg = msg;
    frame("GeometricalOS Setup");
    line(36, "Installing GeometricalOS", COL_BLACK);
    line(64, msg, COL_NAVY);
    int bx = dx + 96;
    int by = dy + 120;
    int bw = dw - 120;
    gfx_sunken(bx, by, bw, 22, COL_WHITE);
    int fill = (bw - 4) * pct / 100;
    if (fill > 0) gfx_fill(bx + 2, by + 2, fill, 18, COL_NAVY);
    char num[16];
    int n = 0;
    int v = pct;
    char rev[8]; int r = 0;
    if (v == 0) rev[r++] = '0';
    while (v) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    while (r) num[n++] = rev[--r];
    num[n++] = '%';
    num[n] = 0;
    gfx_text(bx, by + 30, num, COL_BLACK, COL_GRAY);
    present();
    serial_write("SETUP: ");
    serial_write(msg);
    serial_write(" ");
    serial_dec((uint32_t)pct);
    serial_write("\n");
}

static int do_install(const AtaDisk *disk) {
    if (!install_clock_from_rtc &&
        rtc_set_local_with_offset(&install_clock, install_timezone) < 0)
        return 12;
    progress_ui(8, "Preparing the disk...");
    if (fs_format(disk->sectors)) return 1;
    if (settings_set_timezone_minutes(install_timezone) < 0) return 13;
    progress_ui(22, "Writing welcome files...");
    static const char readme[] =
        "Welcome to GeometricalOS!\r\n"
        "\r\n"
        "This file lives on the computer's hard disk.\r\n"
        "Open it from File Manager, or from Notepad's\r\n"
        "File menu. You can edit it and save it back.\r\n"
        "\r\n"
        "Have fun.\r\n";
    if (fs_write(FS_ROOT, "README.TXT", readme, (uint32_t)strlen(readme)) < 0) return 2;
    char userdat[64];
    int up = 0;
    for (int i = 0; acct_name[i] && up < 27; i++) userdat[up++] = acct_name[i];
    userdat[up++] = '\n';
    for (int i = 0; acct_pass[i] && up < 60; i++) userdat[up++] = acct_pass[i];
    userdat[up++] = '\n';
    if (fs_write(FS_ROOT, "USER.DAT", userdat, (uint32_t)up) < 0) return 2;
    if (fs_write(FS_ROOT, "WALL.CFG", "0\n", 2) < 0) return 2;
    int notes = fs_create_dir(FS_ROOT, "NOTES");
    if (notes < 0) return 3;
    static const char welcome[] =
        "Hello from the NOTES folder.\r\n"
        "\r\n"
        "If you can read this, Setup copied files\r\n"
        "onto the IDE hard disk.\r\n";
    if (fs_write((uint16_t)notes, "WELCOME.TXT", welcome, (uint32_t)strlen(welcome)) < 0) return 4;
    char check[128];
    uint32_t got = 0;
    int id = fs_lookup(FS_ROOT, "README.TXT");
    if (id < 0 || fs_read(id, check, sizeof(check) - 1, &got)) return 5;
    if (got < 8 || memcmp(check, "Welcome", 7) != 0) return 5;

    progress_ui(40, "Copying the system...");
    uint32_t bytes = (uint32_t)((uintptr_t)kernel_file_end - 0x10000u);
    uint32_t sectors = (bytes + 511u) / 512u;
    if (sectors < 4 || sectors > 900) return 6;
    const uint8_t *image = (const uint8_t *)0x10000u;
    for (uint32_t s = 0; s < sectors;) {
        uint32_t count = sectors - s;
        if (count > 16) count = 16;
        if (ata_write(64u + s, image + s * 512u, count)) return 7;
        s += count;
        if ((s & 31u) == 0 || s == sectors)
            progress_ui(40 + (int)((s * 40u) / sectors), "Copying the system...");
    }
    uint8_t sec[512];
    if (ata_read(64, sec, 1) || memcmp(sec, image, 512) != 0) return 8;
    if (ata_read(64 + sectors - 1, sec, 1) ||
        memcmp(sec, image + (sectors - 1) * 512u, 512) != 0) return 8;

    progress_ui(88, "Installing the boot loader...");
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    uint32_t ncopy = mbr_template_len > 512 ? 512 : mbr_template_len;
    memcpy(mbr, mbr_template, ncopy);
    int patched = 0;
    for (uint32_t i = 0; i + 10 < 440; i++) {
        if (mbr[i] == 'G' && mbr[i + 1] == 'E' && mbr[i + 2] == 'O' && mbr[i + 3] == 'L') {
            uint32_t lba = 64;
            uint16_t count = (uint16_t)sectors;
            memcpy(mbr + i + 4, &lba, 4);
            memcpy(mbr + i + 8, &count, 2);
            patched = 1;
            break;
        }
    }
    if (!patched) return 9;
    memset(mbr + 446, 0, 64);
    mbr[446] = 0x80;
    mbr[450] = 0x60;
    mbr[451] = 0xFE;
    mbr[452] = 0xFF;
    mbr[453] = 0xFF;
    uint32_t part_start = 2048;
    uint32_t part_count = disk->sectors > part_start ? disk->sectors - part_start : 0;
    memcpy(mbr + 454, &part_start, 4);
    memcpy(mbr + 458, &part_count, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (ata_write(0, mbr, 1)) return 10;
    if (ata_read(0, sec, 1) || memcmp(sec, mbr, 512) != 0) return 11;
    progress_ui(100, "Finishing...");
    serial_write("SETUP: INSTALLED sectors ");
    serial_dec(sectors);
    serial_write("\n");
    return 0;
}

static void draw_welcome(void) {
    frame("GeometricalOS Setup");
    line(34, "Welcome to Setup", COL_BLACK);
    line(58, "Enter continues. Exit restarts.", COL_NAVY);
    line(86, "Only an IDE disk can be erased,", COL_BLACK);
    line(104, "and only after you confirm its name.", COL_BLACK);
    line(132, "A Windows SSD is not that disk.", COL_BLACK);
    line(160, "No IDE disk: Start runs in memory.", COL_NAVY);
    draw_buttons("Continue", "Exit");
    present();
}

static void draw_live(void) {
    frame("GeometricalOS Setup");
    line(34, "No IDE hard disk was found.", COL_BLACK);
    line(58, "The Windows disk was not changed.", COL_NAVY);
    line(82, "This USB stick is only for booting.", COL_BLACK);
    line(110, "Start opens a temporary desktop.", COL_BLACK);
    line(128, "It stays in memory until restart.", COL_BLACK);
    line(156, "Exit shuts down. Then boot Windows.", COL_NAVY);
    line(174, "On an HP, press Esc, then F9.", COL_NAVY);
    draw_buttons("Start", "Exit");
    present();
}

static void draw_disk(const AtaDisk *disk, const char *err) {
    frame("GeometricalOS Setup");
    if (err) {
        line(34, "Setup needs an IDE hard disk", COL_BLACK);
        line(58, err, COL_MAROON);
        line(96, "Use an IDE controller, not SATA.", COL_BLACK);
        line(114, "Hard disk: Primary Master.", COL_BLACK);
        line(132, "CD: Secondary Master.", COL_BLACK);
        line(160, "Fix the computer, then press ENTER.", COL_NAVY);
        draw_buttons("Retry", "Exit");
    } else {
        line(34, "Setup will erase this disk:", COL_BLACK);
        line(62, disk->model[0] ? disk->model : "IDE hard disk", COL_NAVY);
        char size[48];
        uint32_t mb = disk->sectors / 2048u;
        strcpy(size, "Size: ");
        char rev[12]; int r = 0; uint32_t v = mb;
        if (!v) rev[r++] = '0';
        while (v && r < 10) { rev[r++] = (char)('0' + v % 10); v /= 10; }
        int n = (int)strlen(size);
        while (r) size[n++] = rev[--r];
        strcpy(size + n, " MB");
        line(80, size, COL_NAVY);
        line(108, "All files on it will be lost.", COL_MAROON);
        line(136, "Press ENTER to install.", COL_BLACK);
        draw_buttons("Install", "Back");
    }
    present();
}

static void draw_done(void) {
    frame("GeometricalOS Setup");
    line(34, "GeometricalOS is now installed.", COL_BLACK);
    line(62, "You may remove the installation disk.", COL_BLACK);
    line(88, "Press ENTER to restart.", COL_BLACK);
    line(116, "At startup, press a key for the disk,", COL_NAVY);
    line(134, "or wait to boot from the hard drive.", COL_NAVY);
    draw_buttons("Restart", 0);
    present();
}

static void draw_clock_setup(void) {
    frame("GeometricalOS Setup");
    line(34, "Date and time", COL_BLACK);
    line(62, setup_vm_detected ? "Virtual machine detected; using its RTC clock."
                               : "Using the computer's hardware RTC clock.",
         COL_NAVY);
    char stamp[20], zone[10];
    put4(stamp, install_clock.year); stamp[4] = '-'; put2(stamp + 5, install_clock.month);
    stamp[7] = '-'; put2(stamp + 8, install_clock.day); stamp[10] = ' ';
    put2(stamp + 11, install_clock.hour); stamp[13] = ':';
    put2(stamp + 14, install_clock.min); stamp[16] = ':';
    put2(stamp + 17, install_clock.sec); stamp[19] = 0;
    format_timezone(zone, install_timezone);
    line(88, "Current clock:", COL_BLACK);
    gfx_text(dx + 208, dy + 88, stamp, COL_NAVY, COL_GRAY);
    line(114, "UTC offset:", COL_BLACK);
    gfx_text(dx + 192, dy + 114, zone, COL_NAVY, COL_GRAY);
    line(142, "RTC supplies date/time, not the time zone.", COL_BLACK);
    line(160, "Continue accepts these values; Change to edit.", COL_BLACK);
    draw_buttons("Continue", "Change");
    present();
}

static int configure_clock(void) {
    setup_vm_detected = detect_virtual_machine();
    rtc_read(&install_clock);
    install_timezone = settings_timezone_minutes();
    install_clock_from_rtc = 1;

    for (;;) {
        draw_clock_setup();
        int choice = 0;
        while (!choice) {
            choice = poll_choice("Continue", "Change");
            if (!choice) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
        }
        if (choice == 1) {
            return 1;
        }

        char date[28], time[28], offset[28];
        put4(date, install_clock.year);
        put2(date + 4, install_clock.month);
        put2(date + 6, install_clock.day);
        date[8] = 0;
        const char *prompt = "Date as YYYYMMDD (2000-2099):";
        int value;
        for (;;) {
            ask_line("SETUP: DATE\n", "Set the date", prompt, date, 0);
            if (parse_digits(date, 8, &value)) {
                install_clock.year = value / 10000;
                install_clock.month = (value / 100) % 100;
                install_clock.day = value % 100;
                if (install_clock.year >= 2000 && install_clock.year <= 2099 &&
                    install_clock.month >= 1 && install_clock.month <= 12 &&
                    install_clock.day >= 1 &&
                    install_clock.day <= days_in_month(install_clock.year, install_clock.month))
                    break;
            }
            prompt = "Invalid date. Use YYYYMMDD, year 2000-2099.";
        }

        put2(time, install_clock.hour);
        put2(time + 2, install_clock.min);
        put2(time + 4, install_clock.sec);
        time[6] = 0;
        prompt = "Time as HHMMSS (24-hour):";
        for (;;) {
            ask_line("SETUP: TIME\n", "Set the time", prompt, time, 0);
            if (parse_digits(time, 6, &value)) {
                install_clock.hour = value / 10000;
                install_clock.min = (value / 100) % 100;
                install_clock.sec = value % 100;
                if (install_clock.hour <= 23 &&
                    install_clock.min <= 59 && install_clock.sec <= 59)
                    break;
            }
            prompt = "Invalid time. Use HHMMSS in 24-hour time.";
        }

        format_offset_input(offset, install_timezone);
        prompt = "UTC offset minutes (-720..840, step 15):";
        for (;;) {
            ask_line("SETUP: ZONE\n", "Set the time zone", prompt, offset, 0);
            if (parse_timezone(offset, &install_timezone) &&
                rtc_local_valid_for_offset(&install_clock, install_timezone))
                break;
            prompt = "Invalid zone/date. Use -720..840, step 15.";
        }

        install_clock_from_rtc = 0;
        return 1;
    }
}

static int start_live(void) {
    if (ata_ramdisk_begin()) return 20;
    AtaDisk disk;
    if (ata_probe(&disk) || fs_format(disk.sectors)) return 21;
    if (settings_set_timezone_minutes(install_timezone) < 0) return 13;
    static const char readme[] =
        "Welcome to GeometricalOS.\r\n"
        "\r\n"
        "This desktop is running from memory.\r\n"
        "The USB stick and the Windows disk\r\n"
        "were not changed. Restarting clears\r\n"
        "these files.\r\n";
    if (fs_write(FS_ROOT, "README.TXT", readme, (uint32_t)strlen(readme)) < 0) return 2;
    char userdat[64];
    int up = 0;
    for (int i = 0; acct_name[i] && up < 27; i++) userdat[up++] = acct_name[i];
    userdat[up++] = '\n';
    for (int i = 0; acct_pass[i] && up < 60; i++) userdat[up++] = acct_pass[i];
    userdat[up++] = '\n';
    if (fs_write(FS_ROOT, "USER.DAT", userdat, (uint32_t)up) < 0) return 2;
    if (fs_write(FS_ROOT, "WALL.CFG", "0\n", 2) < 0) return 2;
    int notes = fs_create_dir(FS_ROOT, "NOTES");
    if (notes < 0) return 3;
    static const char welcome[] = "Notes from the temporary desktop.\r\n";
    if (fs_write((uint16_t)notes, "WELCOME.TXT", welcome, (uint32_t)strlen(welcome)) < 0) return 4;
    serial_write("SETUP: LIVE\n");
    return 0;
}

static void draw_fail(int code) {
    frame("GeometricalOS Setup");
    line(34, "Setup could not finish.", COL_MAROON);
    char buf[32] = "Error code: ";
    int n = (int)strlen(buf);
    if (code >= 10) buf[n++] = (char)('0' + code / 10);
    buf[n++] = (char)('0' + code % 10);
    buf[n] = 0;
    line(62, buf, COL_BLACK);
    if (code == 12) {
        line(96, "The requested date/time could not be set.", COL_BLACK);
        line(114, "Check the date and UTC offset.", COL_BLACK);
    } else if (code == 13) {
        line(96, "Time zone settings could not be saved.", COL_BLACK);
        line(114, "Check that the disk is writable.", COL_BLACK);
    } else {
        line(96, "The disk may be missing, too small,", COL_BLACK);
        line(114, "or not the IDE primary master.", COL_BLACK);
    }
    draw_buttons("Back", "Exit");
    present();
}

static int ask_line(const char *marker, const char *heading, const char *prompt, char *buf, int secret) {
    int len = (int)strlen(buf);
    int button_armed = 0;
    serial_write(marker);
    for (;;) {
        frame("GeometricalOS Setup");
        line(34, heading, COL_BLACK);
        line(62, prompt, COL_NAVY);
        gfx_sunken(dx + 96, dy + 110, dw - 130, 26, COL_WHITE);
        char shown[28];
        if (secret) {
            for (int i = 0; i < len && i < 27; i++) shown[i] = '*';
            shown[len < 27 ? len : 27] = 0;
        } else {
            strncpy(shown, buf, 28);
        }
        gfx_text(dx + 102, dy + 114, shown, COL_BLACK, COL_WHITE);
        line(160, "ENTER accepts. ESC clears the field.", COL_BLACK);
        int bw = 120;
        int by = dy + dh - 42;
        int bx = dx + dw - 18 - bw;
        button_state(bx, by, bw, "OK", button_armed);
        present();
        input_poll();
        int go = 0;
        MouseEvent m;
        while (input_mouse(&m)) {
            if (m.type == MOUSE_LDOWN) {
                if (point_in(m.x, m.y, bx, by, bw, 26)) {
                    button_armed = 1;
                    button_state(bx, by, bw, "OK", 1);
                    present();
                }
            } else if (m.type == MOUSE_LUP && button_armed) {
                int inside = point_in(m.x, m.y, bx, by, bw, 26);
                button_armed = 0;
                button_state(bx, by, bw, "OK", 0);
                present();
                if (inside) go = 1;
            }
        }
        KeyEvent k;
        while (input_key(&k)) {
            if (k.code == 10 || k.code == 13) {
                button_state(bx, by, bw, "OK", 1);
                present();
                button_state(bx, by, bw, "OK", 0);
                present();
                go = 1;
            }
            else if (k.code == 27) { len = 0; buf[0] = 0; }
            else if (k.code == 8 && len > 0) buf[--len] = 0;
            else if (!secret && k.code >= 32 && k.code < 127 && len < 20) {
                buf[len++] = (char)k.code;
                buf[len] = 0;
            } else if (secret && k.code >= 32 && k.code < 127 && len < 20) {
                buf[len++] = (char)k.code;
                buf[len] = 0;
            }
        }
        if (go) {
            if (!secret && !buf[0]) {
                strcpy(buf, "User");
                len = 4;
            }
            return 0;
        }
        cursor_restore();
        cursor_draw(mouse_x, mouse_y);
    }
}

int run_setup(void) {
    sound_startup();
    serial_write("SETUP: WELCOME\n");
    for (;;) {
        draw_welcome();
        int c = 0;
        while (!c) {
            c = poll_choice("Continue", "Exit");
            if (!c) {
                cursor_restore();
                cursor_draw(mouse_x, mouse_y);
            }
        }
        if (c == 2) {
            serial_write("SETUP: CANCEL\n");
            return 1;
        }
        ask_line("SETUP: NAME\n", "Who will use this computer?", "Your name:", acct_name, 0);
        ask_line("SETUP: PASS\n", "Choose a password, or leave it blank.", "Password (optional):", acct_pass, 1);
        for (;;) {
            AtaDisk disk;
            int pr = ata_probe(&disk);
            if (pr != 0 || disk.sectors < 16384) {
                const char *err = "No hard disk on Primary Master.";
                if (pr == -2) err = "Primary Master is the CD drive.";
                else if (pr == 0 && disk.sectors < 16384) err = "The disk is smaller than 8 MB.";
                serial_write("SETUP: NODISK\n");
                if (pr != 0) {
                    draw_live();
                    int d = 0;
                    while (!d) {
                        d = poll_choice("Start", "Exit");
                        if (!d) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
                    }
                    if (d == 2) return 1;
                    configure_clock();
                    int live = start_live();
                    if (live) {
                        serial_write("SETUP: LIVEFAIL\n");
                        draw_fail(live);
                        int e = 0;
                        while (!e) {
                            e = poll_choice("Back", "Exit");
                            if (!e) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
                        }
                        if (e == 2) return 1;
                        continue;
                    }
                    return 2;
                }
                draw_disk(0, err);
                int d = 0;
                while (!d) {
                    d = poll_choice("Retry", "Exit");
                    if (!d) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
                }
                if (d == 2) return 1;
                continue;
            }
            serial_write("SETUP: CONFIRM\n");
            draw_disk(&disk, 0);
            int d = 0;
            while (!d) {
                d = poll_choice("Install", "Back");
                if (!d) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
            }
            if (d == 2) break;
            configure_clock();
            int rc = do_install(&disk);
            if (rc) {
                serial_write("SETUP: ERROR\n");
                draw_fail(rc);
                int e = 0;
                while (!e) {
                    e = poll_choice("Back", "Exit");
                    if (!e) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
                }
                if (e == 2) return 1;
                continue;
            }
            serial_write("SETUP: DONE\n");
            for (;;) {
                draw_done();
                int e = 0;
                while (!e) {
                    e = poll_choice("Restart", 0);
                    if (!e) { cursor_restore(); cursor_draw(mouse_x, mouse_y); }
                }
                return 0;
            }
        }
    }
}
