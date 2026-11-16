#include "apps.h"
#include "bootinfo.h"
#include "fs.h"
#include "gfx.h"
#include "gui.h"
#include "image_memory.h"
#include "serial.h"
#include "string.h"
#include "third_party/libwebp/src/webp/decode.h"
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb/stb_image.h"

#define IMAGE_INPUT_ADDRESS 0x790000u
#define IMAGE_PIXELS_ADDRESS 0x1700000u
#define IMAGE_MAX_PIXELS 2100000u
#define IMAGE_RGBA_BYTES (IMAGE_MAX_PIXELS * 4u)

static Window *image_window;
static uint8_t *image_pixels;
static int image_width;
static int image_height;
static char image_filename[28];

static int image_extension(const char *name) {
    int n = (int)strlen(name);
    if (n >= 4 && strcasecmp(name + n - 4, ".PNG") == 0) return 1;
    if (n >= 4 && strcasecmp(name + n - 4, ".JPG") == 0) return 2;
    if (n >= 5 && strcasecmp(name + n - 5, ".JPEG") == 0) return 2;
    if (n >= 5 && strcasecmp(name + n - 5, ".WEBP") == 0) return 3;
    return 0;
}

static int image_dimensions_ok(int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    return (uint64_t)(uint32_t)width * (uint32_t)height <= IMAGE_MAX_PIXELS;
}

static int image_memory_available(void) {
    const BootInfo *info = bootinfo();
    if (info->magic != BOOTINFO_MAGIC) return 0;
    uint64_t memory_end = 0x100000ull + (uint64_t)info->ext_mem_kb * 1024u;
    return memory_end >= (uint64_t)IMAGE_PIXELS_ADDRESS + IMAGE_RGBA_BYTES;
}

static int decode_image(int inode, uint8_t **pixels, int *width, int *height) {
    int format = image_extension(fs_name(inode));
    if (!format || !image_memory_available()) return 0;

    uint32_t file_size = fs_size(inode);
    if (file_size == 0 || file_size > FS_MAX_FILE_BYTES) return 0;
    uint8_t *input = (uint8_t *)IMAGE_INPUT_ADDRESS;
    uint32_t read_size = 0;
    if (fs_read(inode, input, FS_MAX_FILE_BYTES, &read_size) != 0 ||
        read_size != file_size)
        return 0;

    image_memory_reset();
    int w = 0, h = 0;
    uint8_t *output = (uint8_t *)IMAGE_PIXELS_ADDRESS;
    if (format == 3) {
        if (!WebPGetInfo(input, (size_t)file_size, &w, &h) ||
            !image_dimensions_ok(w, h))
            return 0;
        size_t stride = (size_t)w * 4u;
        size_t output_size = stride * (size_t)h;
        if (output_size > IMAGE_RGBA_BYTES ||
            !WebPDecodeRGBAInto(input, (size_t)file_size, output,
                                output_size, (int)stride))
            return 0;
    } else {
        int components = 0;
        if (!stbi_info_from_memory(input, (int)file_size, &w, &h, &components) ||
            !image_dimensions_ok(w, h))
            return 0;
        uint8_t *decoded = stbi_load_from_memory(
            input, (int)file_size, &w, &h, &components, 4);
        if (!decoded) return 0;
        size_t output_size = (size_t)w * (size_t)h * 4u;
        if (output_size > IMAGE_RGBA_BYTES) return 0;
        memcpy(output, decoded, output_size);
        image_memory_reset();
    }

    *pixels = output;
    *width = w;
    *height = h;
    return 1;
}

static void set_title(void) {
    char title[48];
    strncpy(title, image_filename, sizeof(title));
    title[sizeof(title) - 1] = 0;
    strncpy(title + strlen(title), " - Image Viewer",
            sizeof(title) - strlen(title));
    if (image_window) strncpy(image_window->title, title, sizeof(image_window->title));
}

static void image_event(Window *window, Event *event) {
    int cx, cy, cw, ch;
    window_client(window, &cx, &cy, &cw, &ch);
    if (event->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy + ch - 20, cw, 20, COL_GRAY);
        if (!image_pixels) {
            gfx_text(cx + 8, cy + 8, "Open a PNG, JPG, JPEG, or WEBP image.",
                     COL_BLACK, COL_WHITE);
            return;
        }

        int view_h = ch - 20;
        int draw_w = cw, draw_h = view_h;
        if ((uint64_t)(uint32_t)cw * (uint32_t)image_height <=
            (uint64_t)(uint32_t)view_h * (uint32_t)image_width) {
            draw_h = (int)((uint64_t)(uint32_t)cw * (uint32_t)image_height /
                           (uint32_t)image_width);
        } else {
            draw_w = (int)((uint64_t)(uint32_t)view_h * (uint32_t)image_width /
                           (uint32_t)image_height);
        }
        if (draw_w < 1) draw_w = 1;
        if (draw_h < 1) draw_h = 1;
        int dx = cx + (cw - draw_w) / 2;
        int dy = cy + (view_h - draw_h) / 2;
        gfx_blit_rgba_scaled(dx, dy, image_pixels, image_width, image_height,
                             draw_w, draw_h);

        char status[48];
        strcpy(status, image_filename);
        int n = (int)strlen(status);
        if (n < (int)sizeof(status) - 1) {
            status[n++] = ' ';
            status[n++] = '-';
            status[n++] = ' ';
            status[n] = 0;
        }
        char dimensions[24];
        int pos = 0;
        uint32_t values[2] = {(uint32_t)image_width, (uint32_t)image_height};
        for (int v = 0; v < 2; v++) {
            char reverse[12];
            int digits = 0;
            uint32_t value = values[v];
            do {
                reverse[digits++] = (char)('0' + value % 10u);
                value /= 10u;
            } while (value && digits < (int)sizeof(reverse));
            while (digits) dimensions[pos++] = reverse[--digits];
            if (v == 0) dimensions[pos++] = 'x';
        }
        dimensions[pos] = 0;
        strncpy(status + n, dimensions, sizeof(status) - (size_t)n);
        status[sizeof(status) - 1] = 0;
        gfx_text(cx + 8, cy + ch - 18, status, COL_BLACK, COL_GRAY);
        return;
    }
    if (event->type == EVT_MENU) {
        if (event->menu_id == 1) {
            uint16_t dir = FS_ROOT;
            char name[28] = "";
            if (gui_file("Open Image", 0, &dir, name, sizeof(name))) {
                int inode = fs_lookup(dir, name);
                if (inode >= 0) app_image_load(inode);
            }
        } else if (event->menu_id == 2) {
            gui_close(window);
        }
    }
}

void app_image_load(int inode) {
    if (inode <= 0 || !fs_type(inode) || fs_is_dir(inode)) return;
    if (!image_memory_available()) {
        gui_dialog("Image Viewer",
                   "Image Viewer needs 32 MB or more of VM memory.",
                   "OK", 0, 0);
        return;
    }
    uint8_t *pixels;
    int width, height;
    if (!decode_image(inode, &pixels, &width, &height)) {
        gui_dialog("Image Viewer",
                   "Could not decode this image. Use PNG, JPG, JPEG, or WEBP "
                   "under 2.1 megapixels.",
                   "OK", 0, 0);
        return;
    }
    if (!image_window) {
        image_window = gui_create("Image Viewer", 640, 460, ICON_IMAGE,
                                  image_event, &image_window);
        if (!image_window) {
            gui_dialog("Image Viewer", "There is no room for another window.",
                       "OK", 0, 0);
            return;
        }
        window_add_menu(image_window, "File");
        window_add_item(image_window, 0, "Open...", 1);
        window_add_item(image_window, 0, "Close", 2);
    }
    image_pixels = pixels;
    image_width = width;
    image_height = height;
    strncpy(image_filename, fs_name(inode), sizeof(image_filename));
    image_filename[sizeof(image_filename) - 1] = 0;
    set_title();
    app_recent_add(inode);
    gui_focus(image_window);
    gui_invalidate();
    serial_write("IMAGE: opened ");
    serial_write(image_filename);
    serial_write("\n");
}

void app_image_open(void) {
    if (image_window) {
        gui_focus(image_window);
        return;
    }
    uint16_t dir = FS_ROOT;
    char name[28] = "";
    if (!gui_file("Open Image", 0, &dir, name, sizeof(name))) return;
    int inode = fs_lookup(dir, name);
    if (inode >= 0) app_image_load(inode);
}
