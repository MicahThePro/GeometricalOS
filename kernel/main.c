#include "apps.h"
#include "gui.h"
#include "bootinfo.h"
#include "gfx.h"
#include "input.h"
#include "fs.h"
#include "serial.h"
#include "io.h"
#include "sound.h"

static void halt_screen(const char *title, const char *a, const char *b) {
    gfx_reset_clip();
    gfx_clear(COL_SETUPBLUE);
    int w = 460, h = 180;
    int x = (gfx_width() - w) / 2;
    int y = (gfx_height() - h) / 2;
    gfx_raised(x, y, w, h, COL_GRAY);
    gfx_fill(x + 3, y + 3, w - 6, 18, COL_NAVY);
    gfx_text(x + 8, y + 4, title, COL_WHITE, COL_NAVY);
    gfx_text(x + 16, y + 40, a, COL_BLACK, COL_GRAY);
    if (b) gfx_text(x + 16, y + 60, b, COL_BLACK, COL_GRAY);
    gfx_present();
    for (;;) input_poll();
}

void kmain(void) {
    cli_if();
    serial_init();
    serial_write("\nGeometricalOS kernel\n");
    const BootInfo *bi = bootinfo();
    serial_write("magic ");
    serial_hex32(bi->magic);
    serial_write(" ");
    serial_dec(bi->width);
    serial_write("x");
    serial_dec(bi->height);
    serial_write("x");
    serial_dec(bi->bpp);
    serial_write(" lfb ");
    serial_hex32(bi->lfb);
    serial_write(" memKB ");
    serial_dec(bi->ext_mem_kb);
    serial_write(bi->from_hdd ? " hdd\n" : " cd\n");

    if (bi->magic != BOOTINFO_MAGIC || gfx_init() != 0) {
        serial_write("graphics init failed\n");
        for (;;) cli_if();
    }
    input_init();
    if (bi->ext_mem_kb && bi->ext_mem_kb < 7000) {
        halt_screen("GeometricalOS", "This virtual machine needs more memory.", "Give it at least 32 MB of RAM.");
    }
    if (!bi->from_hdd) {
        run_setup();
        reboot();
    }
    if (fs_mount() != 0) {
        serial_write("FS: mount failed\n");
        halt_screen("GeometricalOS", "This hard disk is not installed.", "Boot the GeometricalOS CD and run Setup.");
    }
    splash_screen();
    login_screen();
    desktop_init();
    serial_write("DESKTOP: READY\n");
    gui_run();
}
