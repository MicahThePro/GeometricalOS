#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "fs.h"
#include "rtc.h"
#include "input.h"
#include "io.h"
#include "serial.h"
#include "version.h"

#define TLINES 18
#define TCOLS 62

static Window *win;
static char lines[TLINES][TCOLS + 1];
static int nrows;
static char inbuf[TCOLS + 1];
static int inlen;
static uint16_t cwd = FS_ROOT;

static void term_print(const char *s) {
    char row[TCOLS + 1];
    int c = 0;
    row[0] = 0;
    for (const char *p = s;; p++) {
        if (*p == '\n' || *p == 0 || c >= TCOLS) {
            row[c] = 0;
            if (nrows >= TLINES) {
                for (int i = 1; i < TLINES; i++) strcpy(lines[i - 1], lines[i]);
                nrows = TLINES - 1;
            }
            strncpy(lines[nrows++], row, TCOLS + 1);
            c = 0;
            row[0] = 0;
            if (*p == 0) break;
            if (*p == '\n') continue;
        }
        row[c++] = *p;
    }
}

static int skip_sp(const char *s) {
    int i = 0;
    while (s[i] == ' ') i++;
    return i;
}

static void cmd_dir(void) {
    int ids[64];
    int n = fs_list(cwd, ids, 64);
    if (cwd != FS_ROOT) term_print("..");
    if (!n && cwd == FS_ROOT) term_print("(empty)");
    for (int i = 0; i < n; i++) {
        char row[TCOLS + 1];
        const char *nm = fs_name(ids[i]);
        if (fs_is_dir(ids[i])) {
            strcpy(row, nm);
            strncpy(row + strlen(row), "  <DIR>", TCOLS - strlen(row));
        } else {
            uint32_t sz = fs_size(ids[i]);
            char num[12];
            int p = 0;
            if (!sz) num[p++] = '0';
            else {
                char rev[12]; int r = 0;
                while (sz && r < 10) { rev[r++] = (char)('0' + sz % 10); sz /= 10; }
                while (r) num[p++] = rev[--r];
            }
            num[p] = 0;
            strcpy(row, nm);
            int L = (int)strlen(row);
            if (L < 20) {
                while (L < 20 && L < TCOLS) row[L++] = ' ';
                row[L] = 0;
            }
            strncpy(row + strlen(row), num, TCOLS - strlen(row));
        }
        term_print(row);
    }
}

static void run_cmd(char *cmd) {
    int i = skip_sp(cmd);
    char op[16];
    int o = 0;
    while (cmd[i] && cmd[i] != ' ' && o < 14) op[o++] = cmd[i++];
    op[o] = 0;
    i += skip_sp(cmd + i);
    char *arg = cmd + i;
    for (int k = 0; op[k]; k++)
        if (op[k] >= 'A' && op[k] <= 'Z') op[k] = (char)(op[k] - 'A' + 'a');

    if (!op[0]) return;
    if (strcmp(op, "help") == 0) {
        term_print("dir ls  cd  type cat  echo  cls");
        term_print("mkdir  del  ver  date time");
        term_print("shutdown  restart");
    } else if (strcmp(op, "cls") == 0) {
        nrows = 0;
    } else if (strcmp(op, "ver") == 0) {
        term_print("GeometricalOS version " GOS_VERSION);
    } else if (strcmp(op, "dir") == 0 || strcmp(op, "ls") == 0) {
        cmd_dir();
    } else if (strcmp(op, "cd") == 0) {
        if (!arg[0] || strcmp(arg, "\\") == 0 || strcmp(arg, "/") == 0) cwd = FS_ROOT;
        else if (strcmp(arg, "..") == 0) cwd = fs_parent(cwd);
        else {
            int id = fs_lookup(cwd, arg);
            if (id < 0 || !fs_is_dir(id)) term_print("No such folder");
            else cwd = (uint16_t)id;
        }
    } else if (strcmp(op, "type") == 0 || strcmp(op, "cat") == 0) {
        int id = fs_lookup(cwd, arg);
        if (id < 0 || fs_is_dir(id)) term_print("No such file");
        else {
            static char body[1024];
            uint32_t n = 0;
            if (fs_read(id, body, 1023, &n)) term_print("Read error");
            else {
                body[n] = 0;
                for (uint32_t p = 0; p < n; p++) if (body[p] == '\r') body[p] = '\n';
                term_print(body);
            }
        }
    } else if (strcmp(op, "echo") == 0) {
        term_print(arg[0] ? arg : "");
    } else if (strcmp(op, "mkdir") == 0) {
        if (fs_create_dir(cwd, arg) < 0) term_print("Could not create folder");
    } else if (strcmp(op, "del") == 0) {
        int id = fs_lookup(cwd, arg);
        if (id < 0 || fs_recycle(id)) term_print("Could not move to Recycle Bin");
    } else if (strcmp(op, "date") == 0 || strcmp(op, "time") == 0) {
        RtcTime t;
        rtc_read(&t);
        char b[40];
        b[0] = (char)('0' + (t.month / 10) % 10);
        b[1] = (char)('0' + t.month % 10);
        b[2] = '/';
        b[3] = (char)('0' + (t.day / 10) % 10);
        b[4] = (char)('0' + t.day % 10);
        b[5] = '/';
        int y = t.year, yp = 6;
        char yb[8]; int yn = 0;
        if (!y) yb[yn++] = '0';
        while (y && yn < 8) { yb[yn++] = (char)('0' + y % 10); y /= 10; }
        while (yn) b[yp++] = yb[--yn];
        b[yp++] = ' ';
        b[yp++] = (char)('0' + (t.hour / 10) % 10);
        b[yp++] = (char)('0' + t.hour % 10);
        b[yp++] = ':';
        b[yp++] = (char)('0' + (t.min / 10) % 10);
        b[yp++] = (char)('0' + t.min % 10);
        b[yp++] = ':';
        b[yp++] = (char)('0' + (t.sec / 10) % 10);
        b[yp++] = (char)('0' + t.sec % 10);
        b[yp] = 0;
        term_print(b);
    } else if (strcmp(op, "shutdown") == 0) {
        session_shutdown();
    } else if (strcmp(op, "restart") == 0) {
        session_restart();
    } else {
        term_print("Unknown command. Type help.");
    }
}

static void term_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int fh = gfx_font_h();
    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_BLACK);
        for (int i = 0; i < nrows; i++)
            gfx_text(cx + 4, cy + 2 + i * fh, lines[i], COL_LTGREEN, COL_BLACK);
        int y = cy + 2 + nrows * fh;
        gfx_text(cx + 4, y, ">", COL_LTGREEN, COL_BLACK);
        gfx_text(cx + 12, y, inbuf, COL_WHITE, COL_BLACK);
        if ((timer_ticks() / 5) & 1)
            gfx_fill(cx + 12 + inlen * 8, y, 8, fh, COL_LTGREEN);
        return;
    }
    if (e->type != EVT_KEY) return;
    if (e->code == 10 || e->code == 13) {
        char shown[TCOLS + 1];
        strcpy(shown, ">");
        strncpy(shown + 1, inbuf, TCOLS - 1);
        term_print(shown);
        run_cmd(inbuf);
        inlen = 0;
        inbuf[0] = 0;
    } else if (e->code == 8) {
        if (inlen > 0) inbuf[--inlen] = 0;
    } else if (e->code >= 32 && e->code < 127 && inlen < TCOLS) {
        inbuf[inlen++] = (char)e->code;
        inbuf[inlen] = 0;
    }
}

void app_term_open(void) {
    if (win) { gui_restore(win); return; }
    int ww = 560, wh = 380;
    if (ww > gfx_width() - 16) ww = gfx_width() - 16;
    if (wh > gfx_height() - 16) wh = gfx_height() - 16;
    win = gui_create("Terminal", ww, wh, ICON_TERM, term_event, &win);
    if (win) win->text_keys = 1;
    if (nrows == 0) {
        term_print("GeometricalOS Terminal");
        term_print("Type help and press ENTER.");
    }
    serial_write("APP: TERM\n");
}
