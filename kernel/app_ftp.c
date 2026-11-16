#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "net.h"
#include "input.h"
#include "serial.h"
#include "io.h"
#include "pcnet.h"

#define FTP_CONTROL_PORT 21u
#define FTP_PASSIVE_FIRST 50000u
#define FTP_PASSIVE_COUNT 4u
#define FTP_LINE_SIZE 256u
#define FTP_REPLY_COUNT 8u
#define FTP_REPLY_SIZE 128u
#define FTP_CONNECT_TIMEOUT 3000u
#define FTP_TRANSFER_BUFFER ((uint8_t *)0x764000)

enum {
    FTP_XFER_NONE,
    FTP_XFER_LIST,
    FTP_XFER_RETR,
    FTP_XFER_STOR
};

typedef struct {
    Window *window;
    int running;
    int control;
    int data;
    uint16_t directory;
    uint16_t passive_port;
    uint16_t next_internal;
    uint8_t logged_in;
    uint8_t user_seen;
    uint8_t transfer_kind;
    uint8_t transfer_started;
    uint8_t transfer_fin_sent;
    uint8_t upload_overflow;
    uint8_t close_after_replies;
    uint8_t reply_head;
    uint8_t reply_count;
    uint16_t line_length;
    uint8_t line_overflow;
    uint32_t transfer_size;
    uint32_t transfer_position;
    uint32_t transfer_started_at;
    char transfer_name[28];
    char line[FTP_LINE_SIZE];
    char replies[FTP_REPLY_COUNT][FTP_REPLY_SIZE];
    char status[64];
} FtpServer;

static FtpServer server;
static uint16_t next_passive_port = FTP_PASSIVE_FIRST;

static void control_connected(int connection, void *context);
static void control_data(int connection, const uint8_t *bytes, uint32_t length,
                         void *context);
static void control_closed(int connection, void *context);
static void data_connected(int connection, void *context);
static void data_bytes(int connection, const uint8_t *bytes, uint32_t length,
                       void *context);
static void data_closed(int connection, void *context);

static const NetTcpEvents control_events = {
    control_connected, control_data, control_closed
};
static const NetTcpEvents data_events = {
    data_connected, data_bytes, data_closed
};

static void set_status(const char *text) {
    strncpy(server.status, text, sizeof(server.status));
    server.status[sizeof(server.status) - 1] = 0;
}

static void append_text(char *out, uint32_t capacity, uint32_t *position,
                        const char *text) {
    while (*text && *position + 1 < capacity)
        out[(*position)++] = *text++;
    out[*position] = 0;
}

static void append_uint(char *out, uint32_t capacity, uint32_t *position,
                        uint32_t value) {
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < (int)sizeof(digits));
    while (count && *position + 1 < capacity)
        out[(*position)++] = digits[--count];
    out[*position] = 0;
}

static void append_hex4(char *out, uint32_t capacity, uint32_t *position,
                        uint16_t value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 12; shift >= 0; shift -= 4) {
        if (*position + 1 >= capacity) break;
        out[(*position)++] = digits[(value >> shift) & 0x0F];
    }
    out[*position] = 0;
}

static void close_passive_listener(void) {
    if (server.passive_port) {
        net_tcp_unlisten(server.passive_port);
        server.passive_port = 0;
    }
}

static void reset_transfer(void) {
    server.transfer_kind = FTP_XFER_NONE;
    server.transfer_started = 0;
    server.transfer_fin_sent = 0;
    server.upload_overflow = 0;
    server.transfer_size = 0;
    server.transfer_position = 0;
    server.transfer_started_at = 0;
    server.transfer_name[0] = 0;
}

static void close_transfer_data(void) {
    close_passive_listener();
    int data = server.data;
    server.data = 0;
    if (data && net_tcp_active(data))
        net_tcp_close(data);
}

static void stop_server(void) {
    if (!server.running) return;
    server.running = 0;
    net_tcp_unlisten(FTP_CONTROL_PORT);
    close_passive_listener();
    set_status("Server stopped.");
    serial_write("FTP: server stopped\n");
    net_stop();
    server.control = 0;
    server.data = 0;
    server.logged_in = 0;
    server.user_seen = 0;
    server.reply_count = 0;
    server.reply_head = 0;
    reset_transfer();
    if (server.window) gui_invalidate();
}

static int start_server(void) {
    if (server.running) return 0;
    int dir = fs_create_dir(FS_ROOT, "Transfer");
    if (dir < 0 || !fs_is_dir(dir)) {
        set_status("Could not create Transfer folder.");
        return -1;
    }
    int network_error = net_start();
    if (network_error) {
        if (network_error == -1)
            set_status("PCnet not found; choose PCnet-FAST III.");
        else if (network_error == -2)
            set_status("PCnet I/O BAR invalid; check adapter type.");
        else if (network_error == -3)
        {
            char details[64];
            uint32_t position = 0;
            append_text(details, sizeof(details), &position, "INIT fail C0=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_init_csr0());
            append_text(details, sizeof(details), &position, " B20=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_init_bcr20());
            append_text(details, sizeof(details), &position, " PCI=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_pci_command());
            set_status(details);
        }
        else if (network_error == -4)
            set_status("PCI I/O or bus mastering could not be enabled.");
        else if (network_error == -5)
        {
            char details[64];
            uint32_t position = 0;
            append_text(details, sizeof(details), &position, "Style fail B20=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_init_bcr20());
            append_text(details, sizeof(details), &position, " C0=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_init_csr0());
            append_text(details, sizeof(details), &position, " PCI=");
            append_hex4(details, sizeof(details), &position,
                        pcnet_last_pci_command());
            set_status(details);
        }
        else
            set_status("Network adapter initialization failed.");
        serial_write("FTP: network adapter initialization failed\n");
        return network_error;
    }
    server.directory = (uint16_t)dir;
    if (net_tcp_listen(FTP_CONTROL_PORT, &control_events, 0)) {
        net_stop();
        set_status("Could not start FTP listener.");
        return -3;
    }
    server.running = 1;
    set_status("Waiting for a FileZilla connection.");
    serial_write("FTP: listening on 192.168.56.2:21\n");
    return 0;
}

static void queue_raw(const char *text) {
    if (server.reply_count >= FTP_REPLY_COUNT) {
        set_status("Control reply queue full; closing client.");
        serial_write("FTP: control reply queue full\n");
        if (server.control) net_tcp_close(server.control);
        return;
    }
    uint8_t slot = (uint8_t)((server.reply_head + server.reply_count) %
                             FTP_REPLY_COUNT);
    strncpy(server.replies[slot], text, FTP_REPLY_SIZE);
    server.replies[slot][FTP_REPLY_SIZE - 1] = 0;
    server.reply_count++;
}

static void reply(int code, const char *text) {
    char line[FTP_REPLY_SIZE];
    line[0] = (char)('0' + (code / 100) % 10);
    line[1] = (char)('0' + (code / 10) % 10);
    line[2] = (char)('0' + code % 10);
    line[3] = ' ';
    line[4] = 0;
    uint32_t position = 4;
    append_text(line, sizeof(line), &position, text);
    append_text(line, sizeof(line), &position, "\r\n");
    queue_raw(line);
}

static int safe_filename(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    int length = 0;
    for (const char *p = name; *p; p++, length++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
              c == '_' || c == '-'))
            return 0;
        if (length >= 27) return 0;
    }
    return length > 0;
}

static int internal_name(const char *name) {
    return name && (strncasecmp(name, ".FTPU", 5) == 0 ||
                    strncasecmp(name, ".FTPX", 5) == 0);
}

static int make_internal_name(const char *prefix, const char *suffix,
                              char out[28]) {
    for (uint32_t attempt = 0; attempt < 65536u; attempt++) {
        uint32_t position = 0;
        out[0] = 0;
        append_text(out, 28, &position, prefix);
        append_hex4(out, 28, &position, ++server.next_internal);
        append_text(out, 28, &position, suffix);
        if (fs_lookup(server.directory, out) < 0) return 0;
    }
    return -1;
}

static void queue_size_reply(uint32_t size) {
    char text[FTP_REPLY_SIZE];
    uint32_t position = 0;
    text[0] = 0;
    append_uint(text, sizeof(text), &position, size);
    reply(213, text);
}

static void list_append(const char *name, uint32_t size, int machine) {
    uint32_t position = server.transfer_size;
    uint32_t capacity = FS_MAX_FILE_BYTES;
    char *out = (char *)FTP_TRANSFER_BUFFER;
    if (machine) {
        append_text(out, capacity, &position, "type=file;size=");
        append_uint(out, capacity, &position, size);
        append_text(out, capacity, &position, "; ");
    } else {
        append_text(out, capacity, &position, "-rw-r--r-- 1 geos geos ");
        append_uint(out, capacity, &position, size);
        append_text(out, capacity, &position, " Jan  1 00:00 ");
    }
    append_text(out, capacity, &position, name);
    append_text(out, capacity, &position, "\r\n");
    if (position < capacity) server.transfer_size = position;
}

static int prepare_listing(int machine) {
    int children[160];
    int count = fs_list(server.directory, children, 160);
    if (count < 0) return -1;
    server.transfer_size = 0;
    for (int i = 0; i < count; i++) {
        int inode = children[i];
        if (fs_type(inode) != 1 || internal_name(fs_name(inode))) continue;
        list_append(fs_name(inode), fs_size(inode), machine);
    }
    server.transfer_position = 0;
    return 0;
}

static int data_peer_matches(int connection) {
    uint8_t control_ip[4], data_ip[4];
    if (!server.control ||
        net_tcp_peer_ipv4(server.control, control_ip) ||
        net_tcp_peer_ipv4(connection, data_ip))
        return 0;
    return memcmp(control_ip, data_ip, sizeof(control_ip)) == 0;
}

static void prepare_data_listener(int extended) {
    if (server.transfer_kind != FTP_XFER_NONE) {
        reply(450, "A transfer is already in progress.");
        return;
    }
    if (server.data) {
        net_tcp_close(server.data);
        server.data = 0;
    }
    close_passive_listener();

    uint16_t selected = 0;
    for (uint32_t i = 0; i < FTP_PASSIVE_COUNT; i++) {
        uint16_t port = next_passive_port++;
        if (next_passive_port >= FTP_PASSIVE_FIRST + FTP_PASSIVE_COUNT)
            next_passive_port = FTP_PASSIVE_FIRST;
        if (port < FTP_PASSIVE_FIRST ||
            port >= FTP_PASSIVE_FIRST + FTP_PASSIVE_COUNT)
            continue;
        if (!net_tcp_listen(port, &data_events, 0)) {
            selected = port;
            break;
        }
    }
    if (!selected) {
        reply(425, "Could not open a passive data port.");
        return;
    }
    server.passive_port = selected;
    set_status("Passive FTP port is ready.");

    char text[FTP_REPLY_SIZE];
    uint32_t position = 0;
    text[0] = 0;
    if (extended) {
        append_text(text, sizeof(text), &position, "Entering Extended Passive Mode (|||");
        append_uint(text, sizeof(text), &position, selected);
        append_text(text, sizeof(text), &position, "|)");
        reply(229, text);
    } else {
        uint8_t address[4];
        net_get_ipv4(address);
        append_text(text, sizeof(text), &position, "Entering Passive Mode (");
        for (int i = 0; i < 4; i++) {
            if (i) append_text(text, sizeof(text), &position, ",");
            append_uint(text, sizeof(text), &position, address[i]);
        }
        append_text(text, sizeof(text), &position, ",");
        append_uint(text, sizeof(text), &position, selected / 256u);
        append_text(text, sizeof(text), &position, ",");
        append_uint(text, sizeof(text), &position, selected % 256u);
        append_text(text, sizeof(text), &position, ")");
        reply(227, text);
    }
}

static int begin_transfer(uint8_t kind, const char *name) {
    if (!server.logged_in) {
        reply(530, "Please log in with USER and PASS.");
        return -1;
    }
    if (!server.passive_port && !server.data) {
        reply(425, "Use PASV or EPSV first.");
        return -1;
    }
    if (server.transfer_kind != FTP_XFER_NONE) {
        reply(450, "A transfer is already in progress.");
        return -1;
    }
    server.transfer_kind = kind;
    server.transfer_started = server.data && net_tcp_active(server.data);
    server.transfer_fin_sent = 0;
    server.upload_overflow = 0;
    server.transfer_position = 0;
    server.transfer_started_at = timer_ticks();
    if (kind == FTP_XFER_STOR) server.transfer_size = 0;
    if (name) strncpy(server.transfer_name, name, sizeof(server.transfer_name));
    else server.transfer_name[0] = 0;
    reply(150, "Opening passive data connection.");
    set_status("Waiting for data connection.");
    return 0;
}

static void handle_list(int machine) {
    if (!server.logged_in) {
        reply(530, "Please log in first.");
        return;
    }
    if (!server.passive_port && !server.data) {
        reply(425, "Use PASV or EPSV first.");
        return;
    }
    if (server.transfer_kind != FTP_XFER_NONE) {
        reply(450, "A transfer is already in progress.");
        return;
    }
    if (prepare_listing(machine)) {
        reply(451, "Could not read the Transfer folder.");
        return;
    }
    if (!begin_transfer(FTP_XFER_LIST, 0)) {
        set_status("Preparing directory listing.");
    }
}

static void handle_retr(const char *name) {
    if (!server.logged_in) {
        reply(530, "Please log in first.");
        return;
    }
    if (!server.passive_port && !server.data) {
        reply(425, "Use PASV or EPSV first.");
        return;
    }
    if (server.transfer_kind != FTP_XFER_NONE) {
        reply(450, "A transfer is already in progress.");
        return;
    }
    if (!safe_filename(name)) {
        reply(550, "Use a file name inside Transfer only.");
        return;
    }
    int inode = fs_lookup(server.directory, name);
    if (inode < 0 || fs_type(inode) != 1) {
        reply(550, "File not found.");
        return;
    }
    uint32_t size = 0;
    if (fs_read(inode, FTP_TRANSFER_BUFFER, FS_MAX_FILE_BYTES, &size) ||
        size != fs_size(inode)) {
        reply(451, "Could not read the requested file.");
        return;
    }
    if (!begin_transfer(FTP_XFER_RETR, name)) {
        server.transfer_size = size;
        set_status("Preparing file download.");
    }
}

static void handle_stor(const char *name) {
    if (!server.logged_in) {
        reply(530, "Please log in first.");
        return;
    }
    if (!server.passive_port && !server.data) {
        reply(425, "Use PASV or EPSV first.");
        return;
    }
    if (server.transfer_kind != FTP_XFER_NONE) {
        reply(450, "A transfer is already in progress.");
        return;
    }
    if (!safe_filename(name) || internal_name(name)) {
        reply(550, "Use a valid file name inside Transfer only.");
        return;
    }
    int existing = fs_lookup(server.directory, name);
    if (existing >= 0 && fs_type(existing) != 1) {
        reply(550, "A folder has that name.");
        return;
    }
    if (begin_transfer(FTP_XFER_STOR, name)) return;
    set_status("Waiting for file upload.");
}

static int parse_root_path(const char *path) {
    return !path[0] || !strcmp(path, "/") || !strcmp(path, ".");
}

static void handle_command(char *line) {
    char *argument = line;
    while (*argument && *argument != ' ' && *argument != '\t') {
        if (*argument >= 'a' && *argument <= 'z')
            *argument = (char)(*argument - 'a' + 'A');
        argument++;
    }
    if (*argument) *argument++ = 0;
    while (*argument == ' ' || *argument == '\t') argument++;
    uint32_t arg_length = (uint32_t)strlen(argument);
    while (arg_length && (argument[arg_length - 1] == ' ' ||
                          argument[arg_length - 1] == '\t'))
        argument[--arg_length] = 0;
    if (!line[0]) return;

    if (!strcmp(line, "USER")) {
        if (!strcasecmp(argument, "anonymous") || !strcasecmp(argument, "ftp")) {
            server.user_seen = 1;
            server.logged_in = 0;
            reply(331, "Anonymous access; send any password.");
        } else {
            server.user_seen = 0;
            server.logged_in = 0;
            reply(530, "Only anonymous access is enabled.");
        }
    } else if (!strcmp(line, "PASS")) {
        if (!server.user_seen) reply(503, "Send USER first.");
        else {
            server.logged_in = 1;
            reply(230, "Logged in; files are restricted to Transfer.");
            set_status("Anonymous client logged in.");
        }
    } else if (!strcmp(line, "SYST")) {
        reply(215, "UNIX Type: L8");
    } else if (!strcmp(line, "FEAT")) {
        queue_raw("211-Features\r\n");
        queue_raw(" EPSV\r\n");
        queue_raw(" PASV\r\n");
        queue_raw(" MLSD\r\n");
        queue_raw(" SIZE\r\n");
        queue_raw("211 End\r\n");
    } else if (!strcmp(line, "OPTS")) {
        reply(200, "Options accepted.");
    } else if (!strcmp(line, "TYPE")) {
        if (!strcasecmp(argument, "I") || !strcasecmp(argument, "A") ||
            !strncasecmp(argument, "A ", 2))
            reply(200, "Type set.");
        else
            reply(504, "That transfer type is not supported.");
    } else if (!strcmp(line, "MODE")) {
        reply(!strcasecmp(argument, "S") ? 200 : 504,
              !strcasecmp(argument, "S") ? "Stream mode." : "Use stream mode.");
    } else if (!strcmp(line, "STRU")) {
        reply(!strcasecmp(argument, "F") ? 200 : 504,
              !strcasecmp(argument, "F") ? "File structure." : "Use file structure.");
    } else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD")) {
        reply(257, "\"/\" is the Transfer folder.");
    } else if (!strcmp(line, "CWD") || !strcmp(line, "XCWD") ||
               !strcmp(line, "CDUP") || !strcmp(line, "XCUP")) {
        if (!strcmp(line, "CDUP") || !strcmp(line, "XCUP") ||
            parse_root_path(argument))
            reply(250, "Transfer folder selected.");
        else
            reply(550, "Only the Transfer folder is accessible.");
    } else if (!strcmp(line, "PASV")) {
        if (!server.logged_in) reply(530, "Please log in first.");
        else prepare_data_listener(0);
    } else if (!strcmp(line, "EPSV")) {
        if (!server.logged_in) reply(530, "Please log in first.");
        else prepare_data_listener(1);
    } else if (!strcmp(line, "LIST") || !strcmp(line, "NLST") ||
               !strcmp(line, "MLSD")) {
        if (!server.logged_in) reply(530, "Please log in first.");
        else if (!parse_root_path(argument))
            reply(550, "Only the Transfer folder can be listed.");
        else
            handle_list(!strcmp(line, "MLSD"));
    } else if (!strcmp(line, "RETR")) {
        handle_retr(argument);
    } else if (!strcmp(line, "STOR")) {
        handle_stor(argument);
    } else if (!strcmp(line, "SIZE")) {
        if (!server.logged_in) reply(530, "Please log in first.");
        else if (!safe_filename(argument)) reply(550, "Invalid file name.");
        else {
            int inode = fs_lookup(server.directory, argument);
            if (inode < 0 || fs_type(inode) != 1) reply(550, "File not found.");
            else queue_size_reply(fs_size(inode));
        }
    } else if (!strcmp(line, "NOOP")) {
        reply(200, "OK.");
    } else if (!strcmp(line, "QUIT")) {
        reply(221, "Goodbye.");
        server.close_after_replies = 1;
    } else if (!strcmp(line, "AUTH")) {
        reply(502, "TLS is not supported; configure FileZilla for plain FTP.");
    } else if (!strcmp(line, "PORT") || !strcmp(line, "EPRT")) {
        reply(502, "Active mode is disabled; use passive mode.");
    } else {
        reply(502, "Command not supported.");
    }
}

static void control_connected(int connection, void *context) {
    (void)context;
    if (!server.running) {
        net_tcp_close(connection);
        return;
    }
    if (server.control) {
        int previous = server.control;
        server.control = 0;
        close_transfer_data();
        reset_transfer();
        server.logged_in = 0;
        server.user_seen = 0;
        server.reply_count = 0;
        server.reply_head = 0;
        net_tcp_close(previous);
        serial_write("FTP: replaced previous control session\n");
    }
    server.control = connection;
    server.logged_in = 0;
    server.user_seen = 0;
    server.line_length = 0;
    server.line_overflow = 0;
    server.close_after_replies = 0;
    server.reply_head = 0;
    server.reply_count = 0;
    reset_transfer();
    queue_raw("220 GeometricalOS FTP; anonymous access; Transfer only.\r\n");
    set_status("FTP client connected.");
}

static void control_data(int connection, const uint8_t *bytes, uint32_t length,
                         void *context) {
    (void)context;
    if (!server.running || connection != server.control) return;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t c = bytes[i];
        if (c == '\n') {
            if (server.line_overflow) {
                reply(500, "Command line too long.");
            } else {
                if (server.line_length &&
                    server.line[server.line_length - 1] == '\r')
                    server.line_length--;
                server.line[server.line_length] = 0;
                handle_command(server.line);
            }
            server.line_length = 0;
            server.line_overflow = 0;
        } else if (!server.line_overflow) {
            if (!c || (uint32_t)server.line_length + 1u >= FTP_LINE_SIZE) {
                server.line_overflow = 1;
                server.line_length = 0;
            } else {
                server.line[server.line_length++] = (char)c;
            }
        }
    }
}

static void control_closed(int connection, void *context) {
    (void)context;
    if (!server.running || connection != server.control) return;
    server.control = 0;
    server.logged_in = 0;
    server.user_seen = 0;
    server.close_after_replies = 0;
    close_transfer_data();
    reset_transfer();
    server.reply_count = 0;
    server.reply_head = 0;
    set_status("Waiting for a FileZilla connection.");
}

static void data_connected(int connection, void *context) {
    (void)context;
    if (!server.running || !server.control || !data_peer_matches(connection) ||
        server.data) {
        net_tcp_close(connection);
        return;
    }
    server.data = connection;
    server.transfer_started = 1;
    close_passive_listener();
    set_status("Passive data connection established.");
}

static void data_bytes(int connection, const uint8_t *bytes, uint32_t length,
                       void *context) {
    (void)context;
    if (!server.running || connection != server.data ||
        server.transfer_kind != FTP_XFER_STOR)
        return;
    if (server.upload_overflow ||
        length > FS_MAX_FILE_BYTES - server.transfer_size) {
        server.upload_overflow = 1;
        return;
    }
    memcpy(FTP_TRANSFER_BUFFER + server.transfer_size, bytes, length);
    server.transfer_size += length;
}

static int commit_upload(void) {
    char temporary[28], backup[28];
    if (make_internal_name(".FTPU", ".TMP", temporary)) return -1;
    int temporary_inode = fs_write(server.directory, temporary,
                                   FTP_TRANSFER_BUFFER, server.transfer_size);
    if (temporary_inode < 0) return -2;

    int existing = fs_lookup(server.directory, server.transfer_name);
    if (existing < 0) {
        if (fs_rename(temporary_inode, server.transfer_name)) {
            fs_delete(temporary_inode);
            return -3;
        }
        return 0;
    }
    if (fs_type(existing) != 1 || make_internal_name(".FTPX", ".BAK", backup)) {
        fs_delete(temporary_inode);
        return -4;
    }
    if (fs_rename(existing, backup)) {
        fs_delete(temporary_inode);
        return -5;
    }
    if (fs_rename(temporary_inode, server.transfer_name)) {
        if (fs_rename(existing, server.transfer_name))
            serial_write("FTP: upload rollback failed; old file kept under backup name\n");
        fs_delete(temporary_inode);
        return -6;
    }
    if (fs_delete(existing))
        serial_write("FTP: upload complete; old backup could not be removed\n");
    return 0;
}

static void data_closed(int connection, void *context) {
    (void)context;
    if (!server.running || connection != server.data) return;
    if (server.transfer_kind == FTP_XFER_STOR) {
        if (!net_tcp_received_fin(connection)) {
            reply(426, "Upload connection ended before completion.");
            set_status("Upload interrupted; old file kept.");
        } else if (server.upload_overflow) {
            reply(552, "Upload exceeds the GeoFS file-size limit.");
            set_status("Upload rejected: file is too large.");
        } else if (!commit_upload()) {
            reply(226, "Upload complete.");
            set_status("File uploaded to Transfer.");
            serial_write("FTP: upload complete\n");
        } else {
            reply(452, "Could not save uploaded file.");
            set_status("Upload failed; check disk space.");
            serial_write("FTP: upload could not be committed\n");
        }
    } else if (server.transfer_kind != FTP_XFER_NONE &&
               !server.transfer_fin_sent) {
        reply(426, "Data connection closed before transfer completed.");
        set_status("Data transfer was interrupted.");
    }
    reset_transfer();
    server.data = 0;
    close_passive_listener();
    net_tcp_close(connection);
}

static void send_replies(void) {
    while (server.control && server.reply_count &&
           net_tcp_writable(server.control)) {
        char *line = server.replies[server.reply_head];
        uint32_t length = (uint32_t)strlen(line);
        if (net_tcp_send(server.control, line, length)) break;
        server.reply_head = (uint8_t)((server.reply_head + 1) % FTP_REPLY_COUNT);
        server.reply_count--;
    }
    if (server.control && server.close_after_replies && !server.reply_count &&
        net_tcp_writable(server.control)) {
        server.close_after_replies = 0;
        net_tcp_close(server.control);
    }
}

static void pump_download(void) {
    if (!server.data || !server.transfer_started ||
        (server.transfer_kind != FTP_XFER_LIST &&
         server.transfer_kind != FTP_XFER_RETR) ||
        server.transfer_fin_sent || !net_tcp_writable(server.data))
        return;

    if (server.transfer_position < server.transfer_size) {
        uint32_t count = server.transfer_size - server.transfer_position;
        if (count > 512u) count = 512u;
        if (!net_tcp_send(server.data,
                          FTP_TRANSFER_BUFFER + server.transfer_position,
                          count))
            server.transfer_position += count;
        return;
    }

    if (!net_tcp_close(server.data)) {
        server.transfer_fin_sent = 1;
        server.transfer_kind = FTP_XFER_NONE;
        close_passive_listener();
        reply(226, "Transfer complete.");
        set_status("Data transfer complete.");
    }
}

static void ftp_poll(void) {
    if (!server.running) return;
    send_replies();
    if (server.transfer_kind != FTP_XFER_NONE && !server.transfer_started &&
        timer_ticks() - server.transfer_started_at > FTP_CONNECT_TIMEOUT) {
        reply(425, "Timed out waiting for the passive data connection.");
        close_passive_listener();
        reset_transfer();
        set_status("Passive connection timed out.");
    }
    pump_download();
}

static int ftp_ask_close(Window *window) {
    (void)window;
    stop_server();
    return 1;
}

static void ftp_event(Window *window, Event *event) {
    int cx, cy, cw, ch;
    window_client(window, &cx, &cy, &cw, &ch);
    if (event->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        gfx_text(cx + 16, cy + 14, "GeometricalOS FTP Server", COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 42, "Host-only address: 192.168.56.2", COL_NAVY, COL_GRAY);
        gfx_text(cx + 16, cy + 62, "Port 21  |  Passive ports 50000-50003", COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 92, "Shared folder: Transfer (files only)", COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 122, server.running ? "Status: Running" : "Status: Stopped",
                 server.running ? COL_GREEN : COL_RED, COL_GRAY);
        gfx_text(cx + 16, cy + 146, server.status, COL_BLACK, COL_GRAY);
        gfx_text(cx + 16, cy + 184, "Anonymous plain FTP; no encryption.", COL_RED, COL_GRAY);
        gfx_text(cx + 16, cy + 204, "Use only on VirtualBox Host-only networking.", COL_BLACK, COL_GRAY);
        gfx_button(cx + 16, cy + ch - 42, 100, 28,
                   server.running ? "Stop Server" : "Start Server", 0);
        return;
    }
    if (event->type == EVT_TIMER) {
        gui_invalidate();
        return;
    }
    if (event->type == EVT_DOWN && event->y >= ch - 42 &&
        event->y < ch - 14 && event->x >= 16 && event->x < 116) {
        if (server.running) stop_server();
        else start_server();
        gui_invalidate();
    } else if (event->type == EVT_KEY && event->code == 27) {
        gui_close(window);
    }
}

void app_ftp_open(void) {
    if (server.window) {
        gui_restore(server.window);
        return;
    }
    int width = 420, height = 300;
    if (width > gfx_width() - 16) width = gfx_width() - 16;
    if (height > gfx_height() - GUI_TASKBAR_HEIGHT - 16)
        height = gfx_height() - GUI_TASKBAR_HEIGHT - 16;
    server.window = gui_create("FTP Server", width, height, ICON_FTP,
                               ftp_event, &server.window);
    if (!server.window) return;
    server.window->ask_close = ftp_ask_close;
    gui_set_timer(server.window, 50);
    if (start_server()) gui_dialog("FTP Server", server.status, "OK", 0, 0);
}

void ftp_server_poll(void) {
    net_poll();
    ftp_poll();
}
