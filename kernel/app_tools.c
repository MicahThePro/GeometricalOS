#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "input.h"
#include "ata.h"
#include "fs.h"
#include "serial.h"
#include "io.h"

#define RECENT_MAX 10
#define SEARCH_MAX 48

typedef struct {
    char magic[8];
    uint8_t count;
    uint8_t reserved[3];
    uint16_t ids[RECENT_MAX];
    char names[RECENT_MAX][28];
} RecentData;

typedef struct {
    const char *name;
    const char *text;
} HelpTopic;

static Window *help_window;
static Window *storage_window;
static int help_selected, help_scroll;
static char storage_status[48];
static struct {
    uint32_t total_sectors;
    uint32_t system_sectors;
    uint32_t metadata_sectors;
    uint32_t file_sectors;
    uint32_t free_sectors;
    uint32_t reserved_sectors;
} storage_usage;
static int storage_usage_valid;
static RecentData recent_data;
static int recent_loaded;

extern char kernel_file_end[];

static int text_contains(const char *text, const char *query) {
    if (!text || !query || !query[0]) return 0;
    for (int i = 0; text[i]; i++) {
        int j = 0;
        while (text[i + j] && query[j]) {
            char a = text[i + j], b = query[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) break;
            j++;
        }
        if (!query[j]) return 1;
    }
    return 0;
}

static int supported_document(int inode) {
    if (inode <= 0 || inode >= 160 || fs_type(inode) != 1) return 0;
    const char *name = fs_name(inode);
    int n = (int)strlen(name);
    if (n >= 4 &&
        (strcasecmp(name + n - 4, ".TXT") == 0 ||
         strcasecmp(name + n - 4, ".WRI") == 0 ||
         strcasecmp(name + n - 4, ".GIM") == 0 ||
         strcasecmp(name + n - 4, ".CRD") == 0 ||
         strcasecmp(name + n - 4, ".PNG") == 0 ||
         strcasecmp(name + n - 4, ".JPG") == 0))
        return 1;
    return n >= 5 &&
           (strcasecmp(name + n - 5, ".JPEG") == 0 ||
            strcasecmp(name + n - 5, ".WEBP") == 0);
}

static int in_recycle_bin(int inode) {
    int bin = fs_lookup(FS_ROOT, "Recycle Bin");
    if (bin < 0) return 0;
    uint16_t parent = fs_parent(inode);
    for (int depth = 0; depth < 160 && parent != FS_ROOT; depth++) {
        if (parent == (uint16_t)bin) return 1;
        parent = fs_parent(parent);
    }
    return 0;
}

static void recent_load(void) {
    if (recent_loaded) return;
    recent_loaded = 1;
    memset(&recent_data, 0, sizeof(recent_data));
    int dir = fs_lookup(FS_ROOT, ".GOS");
    int file = dir >= 0 && fs_is_dir(dir) ?
               fs_lookup((uint16_t)dir, "Recent.DAT") : -1;
    if (file < 0 || fs_is_dir(file)) return;

    RecentData stored;
    uint32_t size = 0;
    if (fs_read(file, &stored, sizeof(stored), &size) ||
        size != sizeof(stored) || memcmp(stored.magic, "GOSREC01", 8) ||
        stored.count > RECENT_MAX)
        return;
    memcpy(recent_data.magic, stored.magic, sizeof(stored.magic));
    for (int i = 0; i < stored.count; i++) {
        int id = stored.ids[i];
        if (id <= 0 || id >= 160 || fs_type(id) != 1 ||
            stored.names[i][27] != 0 ||
            in_recycle_bin(id) ||
            strcasecmp(fs_name(id), stored.names[i]) != 0)
            continue;
        recent_data.ids[recent_data.count] = (uint16_t)id;
        strncpy(recent_data.names[recent_data.count], stored.names[i], 28);
        recent_data.count++;
    }
}

void app_recent_add(int inode) {
    if (!supported_document(inode)) return;
    recent_load();
    uint16_t old_ids[RECENT_MAX];
    char old_names[RECENT_MAX][28];
    int old_count = recent_data.count, out = 1;
    memcpy(old_ids, recent_data.ids, sizeof(old_ids));
    memcpy(old_names, recent_data.names, sizeof(old_names));
    memset(&recent_data, 0, sizeof(recent_data));
    memcpy(recent_data.magic, "GOSREC01", 8);
    recent_data.ids[0] = (uint16_t)inode;
    strncpy(recent_data.names[0], fs_name(inode), 28);
    for (int i = 0; i < old_count && out < RECENT_MAX; i++) {
        if (old_ids[i] == inode) continue;
        recent_data.ids[out] = old_ids[i];
        strncpy(recent_data.names[out], old_names[i], 28);
        out++;
    }
    recent_data.count = (uint8_t)out;

    int dir = fs_create_dir(FS_ROOT, ".GOS");
    if (dir < 0 || fs_write((uint16_t)dir, "Recent.DAT",
                            &recent_data, sizeof(recent_data)) < 0)
        serial_write("RECENT: could not save document history\n");
}

static int collect_matches(const char *query, int exact, int *results,
                           char labels[SEARCH_MAX][28]) {
    uint16_t dirs[160];
    uint8_t matched_dir[160];
    int read = 0, write = 1, count = 0;
    dirs[0] = FS_ROOT;
    matched_dir[0] = 0;
    int bin = fs_lookup(FS_ROOT, "Recycle Bin");

    while (read < write && count < SEARCH_MAX) {
        uint16_t parent = dirs[read];
        int parent_match = matched_dir[read++];
        int children[160];
        int n = fs_list(parent, children, 160);
        for (int i = 0; i < n && count < SEARCH_MAX; i++) {
            int id = children[i];
            const char *name = fs_name(id);
            if (fs_is_dir(id)) {
                if (id == bin || strcasecmp(name, ".GOS") == 0) continue;
                if (write < 160) {
                    dirs[write] = (uint16_t)id;
                    matched_dir[write++] =
                        (uint8_t)(parent_match || text_contains(name, query));
                }
            } else {
                int match = parent_match;
                if (exact)
                    match = strcasecmp(name, query) == 0;
                else if (!match)
                    match = text_contains(name, query);
                if (!match) continue;
                results[count] = id;
                int label_len = (int)strlen(name);
                if (label_len > 18) label_len = 18;
                memcpy(labels[count], name, (size_t)label_len);
                int pos = label_len;
                labels[count][pos++] = ' ';
                labels[count][pos++] = '@';
                labels[count][pos] = 0;
                const char *folder = parent == FS_ROOT ? "root" : fs_name(parent);
                for (int j = 0; folder[j] && pos < 27; j++)
                    labels[count][pos++] = folder[j];
                labels[count][pos] = 0;
                count++;
            }
        }
    }
    return count;
}

void app_geofind_open(void) {
    char query[28] = "";
    if (!gui_prompt("GeoFind", "Find by name, type, or folder:", query,
                    sizeof(query)) || !query[0])
        return;
    int results[SEARCH_MAX];
    char labels[SEARCH_MAX][28];
    int count = collect_matches(query, 0, results, labels);
    if (!count) {
        gui_dialog("GeoFind", "No matching files were found.", "OK", 0, 0);
        return;
    }
    int selected = gui_pick("Search results", labels, count);
    if (selected >= 0 && !app_files_open_inode(results[selected]))
        gui_dialog("GeoFind", "This file type has no associated application.",
                   "OK", 0, 0);
}

void app_recent_open(void) {
    recent_load();
    char labels[RECENT_MAX][28];
    int results[RECENT_MAX], count = 0;
    for (int i = 0; i < recent_data.count; i++) {
        int id = recent_data.ids[i];
        if (id <= 0 || id >= 160 || fs_type(id) != 1 ||
            in_recycle_bin(id) ||
            strcasecmp(fs_name(id), recent_data.names[i]) != 0 ||
            !supported_document(id))
            continue;
        results[count] = id;
        strncpy(labels[count], fs_name(id), sizeof(labels[count]));
        labels[count][27] = 0;
        count++;
    }
    if (!count) {
        gui_dialog("Recent Documents", "No recently opened documents.",
                   "OK", 0, 0);
        return;
    }
    int selected = gui_pick("Recent Documents", labels, count);
    if (selected >= 0) app_files_open_inode(results[selected]);
}

static int run_application(const char *command) {
    if (strcasecmp(command, "File Manager") == 0 || strcasecmp(command, "Files") == 0)
        app_files_open();
    else if (strcasecmp(command, "GeoFind") == 0 || strcasecmp(command, "Search") == 0)
        app_geofind_open();
    else if (strcasecmp(command, "Recent Documents") == 0)
        app_recent_open();
    else if (strcasecmp(command, "Run") == 0)
        app_run_open();
    else if (strcasecmp(command, "Help") == 0)
        app_help_open();
    else if (strcasecmp(command, "Disk Space") == 0 ||
             strcasecmp(command, "Disk Space & Backup") == 0)
        app_storage_open();
    else if (strcasecmp(command, "Notepad") == 0) app_notepad_open();
    else if (strcasecmp(command, "Write") == 0) app_write_open();
    else if (strcasecmp(command, "Calculator") == 0) app_calc_open();
    else if (strcasecmp(command, "Clock") == 0) app_clock_open();
    else if (strcasecmp(command, "Paint") == 0) app_paint_open();
    else if (strcasecmp(command, "Image Viewer") == 0 ||
             strcasecmp(command, "Images") == 0) app_image_open();
    else if (strcasecmp(command, "Terminal") == 0) app_term_open();
    else if (strcasecmp(command, "Minesweeper") == 0) app_mines_open();
    else if (strcasecmp(command, "Solitaire") == 0) app_sol_open();
    else if (strcasecmp(command, "Snake") == 0) app_snake_open();
    else if (strcasecmp(command, "Control Panel") == 0) app_control_open();
    else if (strcasecmp(command, "Media Player") == 0) app_player_open();
    else if (strcasecmp(command, "FTP Server") == 0 ||
             strcasecmp(command, "FTP") == 0) app_ftp_open();
    else if (strcasecmp(command, "Character Map") == 0) app_charmap_open();
    else if (strcasecmp(command, "Cardfile") == 0) app_cardfile_open();
    else if (strcasecmp(command, "System Information") == 0) app_recovery_open();
    else if (strcasecmp(command, "Recycle Bin") == 0) app_recycle_open();
    else return 0;
    return 1;
}

void app_run_open(void) {
    char command[28] = "";
    if (!gui_prompt("Run", "Application or file name:", command,
                    sizeof(command)) || !command[0])
        return;
    if (run_application(command)) return;
    int results[SEARCH_MAX];
    char labels[SEARCH_MAX][28];
    int count = collect_matches(command, 1, results, labels);
    if (!count) {
        gui_dialog("Run", "No application or file with that name was found.",
                   "OK", 0, 0);
        return;
    }
    int selected = count == 1 ? 0 : gui_pick("Matching files", labels, count);
    if (selected >= 0 && !app_files_open_inode(results[selected]))
        gui_dialog("Run", "This file type has no associated application.",
                   "OK", 0, 0);
}

static const HelpTopic help_topics[] = {
    {"File Manager", "Open folders with a double-click; use Up or Backspace to return. "
     "Open supported TXT, WRI, CRD, GIM, and image files. "
     "Drag an item onto a folder to move it, onto the Desktop to show it there, or onto "
     "the Recycle Bin to delete it. Right-click for Open, Rename, Delete, Cut, Copy, "
     "and Paste Here. Edit > Cut/Copy/Paste and F2 rename also work."},
    {"GeoFind", "Searches file names, extensions, and folder names. A matching folder "
     "shows its files. Select a result to open TXT, WRI, GIM, CRD, PNG, JPG, JPEG, or WEBP files."},
    {"Recent Documents", "Lists the last ten supported documents opened from File Manager, "
     "GeoFind, Run, or this list. Recycled, renamed, and missing entries are ignored."},
    {"Run", "Enter an application name such as Notepad or Paint, or an exact document "
     "file name. Run opens supported text, Paint, Cardfile, and image files."},
    {"Disk Space & Backup", "Shows the whole IDE disk divided into boot/system, GeoFS "
     "metadata, files, reserved space, and free GeoFS space. Backups in Backups\\Latest "
     "are on this same disk and cannot protect against disk loss."},
    {"Notepad", "Use File > Open or Save As for plain-text documents. Ctrl+S saves the "
     "current document; unsaved edits are confirmed when closing."},
    {"Write", "Create and format simple rich-text documents, then save them as WRI files. "
     "Open WRI documents from File Manager or the File menu."},
    {"Calculator", "Use the number keys and +, -, *, /, and Enter. Clear resets the current "
     "calculation. The calculator works offline."},
    {"Clock", "Displays the system time and date. The time can be adjusted in Control Panel."},
    {"Paint", "Choose a tool and color, then draw with the mouse. Save pictures as GIM files."},
    {"Image Viewer", "Open PNG, JPG, JPEG, and WEBP pictures from File Manager or the File menu. "
     "Images are fitted to the window; files are limited to 2.1 megapixels. Use a VM with "
     "at least 32 MB of memory."},
    {"Terminal", "Use the built-in command prompt for the commands listed by its Help command. "
     "It does not provide network access."},
    {"Control Panel", "Adjust display, sound, date, and other system settings. Sound defaults "
     "to maximum; use the volume control to lower it."},
    {"Media Player", "Play, pause, or stop one of the built-in tunes. No Internet "
     "connection or streaming service is used."},
    {"FTP Server", "Transfers files with FileZilla over VirtualBox Host-only networking. "
     "The anonymous, unencrypted server exposes only files in the Transfer folder. "
     "Use plain FTP on the private Host-only network only."},
    {"Character Map", "Choose a character to inspect or copy it for use in a text document."},
    {"Cardfile", "Create small cards, browse them, and save them as CRD files."},
    {"System Information", "Shows detected hardware and includes recovery actions for system "
     "settings. Resetting settings does not erase documents."},
    {"Recycle Bin", "Right-click an item to restore it, rename it, or delete it permanently. "
     "You can also restore selected items to their original folders or empty the bin."},
    {"Minesweeper", "Reveal safe squares and flag suspected mines. A number shows how many "
     "mines touch that square."},
    {"Solitaire", "Move cards with the mouse. Use the game controls to draw from the stock "
     "and build foundation piles."},
    {"Snake", "Use the arrow keys to steer. Avoid the walls and your own tail."},
    {"Keyboard shortcuts", "File Manager: Enter opens, Backspace goes up, F2 renames, and "
     "Ctrl+X/C/V cuts, copies, and pastes. Right-click items for common actions. "
     "Most windows support Enter and Escape."},
    {"Common fixes", "If a document will not open, check its extension and available disk "
     "space. Restore deleted files from Recycle Bin. Backups are stored on this same disk."}
};

static void help_show_selected(void) {
    int count = (int)(sizeof(help_topics) / sizeof(help_topics[0]));
    if (help_selected < 0 || help_selected >= count) return;
    gui_dialog(help_topics[help_selected].name, help_topics[help_selected].text,
               "OK", 0, 0);
}

static void help_event(Window *window, Event *event) {
    int cx, cy, cw, ch;
    window_client(window, &cx, &cy, &cw, &ch);
    int count = (int)(sizeof(help_topics) / sizeof(help_topics[0]));
    int rows = (ch - 42) / 18;
    if (rows < 1) rows = 1;
    if (event->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_WHITE);
        gfx_fill(cx, cy, cw, 18, COL_GRAY);
        gfx_text(cx + 8, cy + 1, "Choose a topic and press Enter", COL_BLACK, COL_GRAY);
        if (help_selected < help_scroll) help_scroll = help_selected;
        if (help_selected >= help_scroll + rows) help_scroll = help_selected - rows + 1;
        for (int row = 0; row < rows; row++) {
            int idx = help_scroll + row;
            if (idx >= count) break;
            int y = cy + 20 + row * 18;
            int selected = idx == help_selected;
            if (selected) gfx_fill(cx + 1, y, cw - 2, 18, COL_NAVY);
            gfx_text(cx + 8, y + 1, help_topics[idx].name,
                     selected ? COL_WHITE : COL_BLACK,
                     selected ? COL_NAVY : COL_WHITE);
        }
        return;
    }
    if (event->type == EVT_MENU) {
        if (event->menu_id == 1) gui_close(window);
        return;
    }
    if (event->type == EVT_DOWN && event->y >= 20 && event->y < ch - 4) {
        int idx = help_scroll + (event->y - 20) / 18;
        if (idx >= 0 && idx < count) {
            help_selected = idx;
            if (event->dbl) help_show_selected();
        }
        return;
    }
    if (event->type == EVT_KEY) {
        if (event->code == KEY_UP && help_selected > 0) help_selected--;
        else if (event->code == KEY_DOWN && help_selected + 1 < count) help_selected++;
        else if (event->code == 10 || event->code == 13) help_show_selected();
        else if (event->code == 27) gui_close(window);
    }
}

void app_help_open(void) {
    if (help_window) {
        gui_restore(help_window);
        return;
    }
    int width = 480, height = 390;
    if (width > gfx_width() - 16) width = gfx_width() - 16;
    if (height > gfx_height() - 16) height = gfx_height() - 16;
    help_window = gui_create("GeometricalOS Help", width, height, ICON_HELP,
                             help_event, &help_window);
    if (!help_window) return;
    window_add_menu(help_window, "File");
    window_add_item(help_window, 0, "Close", 1);
}

static void format_size(uint64_t bytes, char *out) {
    uint64_t value;
    const char *unit;
    if (bytes >= 1024ull * 1024ull) {
        value = bytes / (1024ull * 1024ull);
        unit = " MB";
    } else if (bytes >= 1024ull) {
        value = bytes / 1024ull;
        unit = " KB";
    } else {
        value = bytes;
        unit = " B";
    }
    char digits[20];
    int n = 0;
    if (!value) digits[n++] = '0';
    while (value && n < 18) {
        digits[n++] = (char)('0' + (uint32_t)(value % 10));
        value /= 10;
    }
    int pos = 0;
    while (n) out[pos++] = digits[--n];
    while (*unit) out[pos++] = *unit++;
    out[pos] = 0;
}

static int storage_scan(void) {
    AtaDisk disk;
    FsSpaceInfo fs;
    if (ata_probe(&disk) || !disk.present || fs_get_space(&fs)) {
        storage_usage_valid = 0;
        return -1;
    }

    uint32_t kernel_bytes = (uint32_t)((uintptr_t)kernel_file_end - 0x10000u);
    uint32_t kernel_sectors = (kernel_bytes + 511u) / 512u;
    uint64_t kernel_end = 64ull + kernel_sectors;
    uint64_t fs_end = (uint64_t)fs.start_sector + fs.metadata_sectors + fs.data_blocks;
    if (fs.used_blocks > fs.data_blocks || kernel_end > fs.start_sector ||
        fs_end > disk.sectors || fs_end > fs.disk_sectors) {
        storage_usage_valid = 0;
        return -1;
    }

    uint64_t managed_sectors = (uint64_t)fs.metadata_sectors + fs.data_blocks;
    uint64_t accounted_sectors = 1ull + kernel_sectors + managed_sectors;
    if (accounted_sectors > disk.sectors) {
        storage_usage_valid = 0;
        return -1;
    }

    storage_usage.total_sectors = disk.sectors;
    storage_usage.system_sectors = 1u + kernel_sectors;
    storage_usage.metadata_sectors = fs.metadata_sectors;
    storage_usage.file_sectors = fs.used_blocks;
    storage_usage.free_sectors = fs.data_blocks - fs.used_blocks;
    storage_usage.reserved_sectors =
        disk.sectors - (uint32_t)accounted_sectors;
    storage_usage_valid = 1;
    return 0;
}

static void storage_refresh_status(void) {
    if (storage_scan()) {
        strncpy(storage_status, "Could not read disk usage.", sizeof(storage_status));
    } else {
        strncpy(storage_status, "Disk usage refreshed.", sizeof(storage_status));
    }
}

static int create_backup(void) {
    int backups = fs_create_dir(FS_ROOT, "Backups");
    if (backups < 0) return -1;
    int pending = fs_lookup((uint16_t)backups, "Pending");
    if (pending >= 0 && fs_delete_tree(pending)) return -2;
    pending = fs_create_dir((uint16_t)backups, "Pending");
    if (pending < 0) return -3;

    int children[160];
    int count = fs_list(FS_ROOT, children, 160);
    for (int i = 0; i < count; i++) {
        int id = children[i];
        const char *name = fs_name(id);
        if (strcasecmp(name, "Backups") == 0 ||
            strcasecmp(name, "Recycle Bin") == 0)
            continue;
        if (fs_copy(id, (uint16_t)pending)) {
            fs_delete_tree(pending);
            return -4;
        }
    }

    int latest = fs_lookup((uint16_t)backups, "Latest");
    int previous = fs_lookup((uint16_t)backups, "Previous");
    if (previous >= 0 && fs_delete_tree(previous)) {
        fs_delete_tree(pending);
        return -5;
    }
    if (latest >= 0 && fs_rename(latest, "Previous")) {
        fs_delete_tree(pending);
        return -6;
    }
    if (fs_rename(pending, "Latest")) {
        if (latest >= 0) fs_rename(latest, "Latest");
        return -7;
    }
    if (latest >= 0) fs_delete_tree(latest);
    return 0;
}

static void restore_backup(void) {
    int backups = fs_lookup(FS_ROOT, "Backups");
    int latest = backups >= 0 && fs_is_dir(backups) ?
                 fs_lookup((uint16_t)backups, "Latest") : -1;
    if ((latest < 0 || !fs_is_dir(latest)) && backups >= 0 && fs_is_dir(backups))
        latest = fs_lookup((uint16_t)backups, "Previous");
    if (latest < 0 || !fs_is_dir(latest)) {
        gui_dialog("Restore Backup", "No local backup was found.", "OK", 0, 0);
        return;
    }
    if (gui_dialog("Restore Backup",
                   "Merge Backups\\Latest into current files?\n"
                   "Matching files are replaced. This backup is\n"
                   "on the same virtual disk.",
                   "Restore", "Cancel", 0) != 1)
        return;
    if (fs_restore_tree(latest, FS_ROOT)) {
        gui_dialog("Restore Backup",
                   "Restore stopped because of a disk error,\n"
                   "name conflict, or insufficient space. Some\n"
                   "files may already have been restored.",
                   "OK", 0, 0);
        if (storage_scan())
            strncpy(storage_status, "Restore stopped; disk scan failed.", sizeof(storage_status));
        else
            strncpy(storage_status, "Restore stopped; see message.", sizeof(storage_status));
    } else {
        if (storage_scan())
            strncpy(storage_status, "Backup restored; disk scan failed.", sizeof(storage_status));
        else
            strncpy(storage_status, "Backup restored.", sizeof(storage_status));
    }
    gui_invalidate();
}

static void storage_backup(void) {
    int result = create_backup();
    int scan_failed = storage_scan();
    if (result) {
        gui_dialog("Disk Space & Backup",
                   "Could not create the snapshot. Check that the\n"
                   "disk has enough free space and inodes. The\n"
                   "previous snapshot was kept if possible.",
                   "OK", 0, 0);
        strncpy(storage_status, scan_failed ? "Backup failed; disk scan failed." :
                "Backup failed.", sizeof(storage_status));
    } else {
        strncpy(storage_status, scan_failed ? "Backup saved; disk scan failed." :
                "Backup saved to Backups\\Latest.", sizeof(storage_status));
    }
    gui_invalidate();
}

static void storage_row(int x, int y, int value_x, const char *label,
                        uint32_t color, uint32_t sectors) {
    char size[24];
    format_size((uint64_t)sectors * 512u, size);
    gfx_fill(x, y + 3, 10, 10, color);
    gfx_rect(x, y + 3, 10, 10, COL_BLACK);
    gfx_text(x + 16, y, label, COL_BLACK, COL_GRAY);
    gfx_text(value_x - gfx_text_width(size), y, size, COL_BLACK, COL_GRAY);
}

static void storage_draw_bar(int x, int y, int width, uint32_t height) {
    const uint32_t colors[] = {
        COL_BLUE, COL_MAROON, COL_AMBER, COL_LTGREEN, COL_DKGRAY
    };
    const uint32_t sectors[] = {
        storage_usage.system_sectors,
        storage_usage.metadata_sectors,
        storage_usage.file_sectors,
        storage_usage.free_sectors,
        storage_usage.reserved_sectors
    };
    uint64_t accumulated = 0;
    int previous = 0;
    for (int i = 0; i < 5; i++) {
        accumulated += sectors[i];
        int end = (int)((uint64_t)width * accumulated / storage_usage.total_sectors);
        if (end > previous)
            gfx_fill(x + previous, y, end - previous, (int)height, colors[i]);
        previous = end;
    }
    gfx_rect(x, y, width, (int)height, COL_BLACK);
}

static void storage_event(Window *window, Event *event) {
    int cx, cy, cw, ch;
    window_client(window, &cx, &cy, &cw, &ch);
    if (event->type == EVT_PAINT) {
        if (!storage_usage_valid) {
            gfx_fill(cx, cy, cw, ch, COL_GRAY);
            gfx_text(cx + 16, cy + 18, "Could not read disk usage.",
                     COL_MAROON, COL_GRAY);
            return;
        }
        uint32_t used_sectors = storage_usage.system_sectors +
                                storage_usage.metadata_sectors +
                                storage_usage.file_sectors;
        char total_text[24], used_text[24];
        format_size((uint64_t)storage_usage.total_sectors * 512u, total_text);
        format_size((uint64_t)used_sectors * 512u, used_text);
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 16, cy + 12, "Detected IDE disk:", COL_BLACK, COL_GRAY);
        gfx_text(cx + cw - 16 - gfx_text_width(total_text), cy + 12,
                 total_text, COL_BLACK, COL_GRAY);
        storage_draw_bar(cx + 16, cy + 32, cw - 32, 16);
        int value_x = cx + cw - 16;
        storage_row(cx + 16, cy + 58, value_x, "Boot + GeometricalOS",
                    COL_BLUE, storage_usage.system_sectors);
        storage_row(cx + 16, cy + 78, value_x, "GeoFS metadata",
                    COL_MAROON, storage_usage.metadata_sectors);
        storage_row(cx + 16, cy + 98, value_x, "Your files",
                    COL_AMBER, storage_usage.file_sectors);
        storage_row(cx + 16, cy + 118, value_x, "Free GeoFS space",
                    COL_LTGREEN, storage_usage.free_sectors);
        storage_row(cx + 16, cy + 138, value_x, "Reserved / outside GeoFS",
                    COL_DKGRAY, storage_usage.reserved_sectors);
        gfx_text(cx + 16, cy + 166, "Used on disk:", COL_BLACK, COL_GRAY);
        gfx_text(value_x - gfx_text_width(used_text), cy + 166,
                 used_text, COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 192,
                 "Reserved space is not available for GeoFS files.",
                 COL_DKGRAY, COL_GRAY);
        gfx_text(cx + 16, cy + 212,
                 "Keep backups on a separate disk to protect them.",
                 COL_MAROON, COL_GRAY);
        gfx_fill(cx, cy + ch - 24, cw, 24, COL_GRAY);
        if (storage_status[0])
            gfx_text(cx + 8, cy + ch - 22, storage_status, COL_BLACK, COL_GRAY);
        return;
    }
    if (event->type == EVT_MENU) {
        if (event->menu_id == 1) storage_backup();
        else if (event->menu_id == 2) restore_backup();
        else if (event->menu_id == 3) {
            storage_refresh_status();
            gui_invalidate();
        } else if (event->menu_id == 4) gui_close(window);
        return;
    }
    if (event->type == EVT_KEY && event->code == 27) gui_close(window);
}

void app_storage_open(void) {
    if (storage_window) {
        storage_scan();
        gui_restore(storage_window);
        return;
    }
    storage_status[0] = 0;
    storage_scan();
    int width = 480, height = 340;
    if (width > gfx_width() - 16) width = gfx_width() - 16;
    if (height > gfx_height() - 16) height = gfx_height() - 16;
    storage_window = gui_create("Disk Space & Backup", width, height, ICON_STORAGE,
                                storage_event, &storage_window);
    if (!storage_window) return;
    window_add_menu(storage_window, "Disk");
    window_add_item(storage_window, 0, "Create Backup...", 1);
    window_add_item(storage_window, 0, "Restore Backup...", 2);
    window_add_item(storage_window, 0, "Refresh", 3);
    window_add_item(storage_window, 0, "Close", 4);
}
