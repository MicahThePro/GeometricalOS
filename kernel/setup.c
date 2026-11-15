#include "apps.h"
#include "gfx.h"
#include "input.h"
#include "ata.h"
#include "fs.h"
#include "bootinfo.h"
#include "sound.h"
#include "serial.h"
#include "io.h"

static char acct_name[28] = "User";
static char acct_pass[28];

extern const unsigned char mbr_template[];
extern const unsigned int mbr_template_len;
extern char kernel_file_end[];

static int dx, dy, dw, dh;
static int busy_pct;
static const char *busy_msg;

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

static int click_in(int x, int y, int w, int h) {
    return mouse_x >= x && mouse_y >= y && mouse_x < x + w && mouse_y < y + h;
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
    int bw = 120;
    int y = dy + dh - 42;
    int x2 = dx + dw - 18 - bw;
    int x1 = secondary ? x2 - 10 - bw : x2;
    input_poll();
    MouseEvent m;
    while (input_mouse(&m)) {
        if (m.type != MOUSE_LDOWN) continue;
        if (click_in(x1, y, bw, 26)) return 1;
        if (secondary && click_in(x2, y, bw, 26)) return 2;
    }
    KeyEvent k;
    while (input_key(&k)) {
        if (k.code == 10 || k.code == 13) return 1;
        if (k.code == 27) return 2;
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
    progress_ui(8, "Preparing the disk...");
    if (fs_format(disk->sectors)) return 1;
    progress_ui(22, "Writing welcome files...");
    static const char readme[] =
        "Welcome to GeometricalOS!\r\n"
        "\r\n"
        "This file lives on the virtual hard disk.\r\n"
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
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_write(64u + s, image + s * 512u, 1)) return 7;
        if ((s & 31u) == 0)
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
    line(62, "This program installs GeometricalOS", COL_BLACK);
    line(80, "onto your virtual hard disk.", COL_BLACK);
    line(108, "The disk on IDE Primary Master", COL_BLACK);
    line(126, "will be erased.", COL_MAROON);
    line(154, "Press ENTER to continue.", COL_NAVY);
    draw_buttons("Continue", "Exit");
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
        line(160, "Fix the VM, then press ENTER.", COL_NAVY);
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
    line(62, "1. In VirtualBox, choose Devices,", COL_BLACK);
    line(80, "   Optical Drives, Remove disk.", COL_BLACK);
    line(108, "2. Press ENTER to restart.", COL_BLACK);
    line(136, "The next boot uses the hard disk.", COL_NAVY);
    draw_buttons("Restart", 0);
    present();
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
    line(96, "The disk may be missing, too small,", COL_BLACK);
    line(114, "or not the IDE primary master.", COL_BLACK);
    draw_buttons("Back", "Exit");
    present();
}

static int ask_line(const char *marker, const char *heading, const char *prompt, char *buf, int secret) {
    int len = (int)strlen(buf);
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
        draw_buttons("OK", 0);
        present();
        input_poll();
        int go = 0;
        MouseEvent m;
        while (input_mouse(&m)) {
            if (m.type == MOUSE_LDOWN) {
                int bw = 120;
                int y = dy + dh - 42;
                int x2 = dx + dw - 18 - bw;
                if (click_in(x2, y, bw, 26)) go = 1;
            }
        }
        KeyEvent k;
        while (input_key(&k)) {
            if (k.code == 10 || k.code == 13) go = 1;
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
