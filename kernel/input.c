#include "input.h"
#include "gfx.h"
#include "io.h"

int mouse_x, mouse_y, mouse_buttons;

static int shift, ctrl, alt, caps, e0;
static uint32_t ticks;
static uint16_t pit_last;
static int pit_on;

static KeyEvent kq[32];
static int kq_r, kq_w, kq_n;
static MouseEvent mq[32];
static int mq_r, mq_w, mq_n;

static const char map_lo[128] = {
    /*00*/ 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', 8,  9,
    /*10*/ 'q','w','e','r','t','y','u','i','o','p','[',']', 10, 0,'a','s',
    /*20*/ 'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    /*30*/ 'b','n','m',',','.','/', 0, '*', 0, ' ', 0,  0,  0,  0,  0,  0,
    /*40*/ 0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    /*50*/ '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /*60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*70*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const char map_hi[128] = {
    /*00*/ 0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', 8,  9,
    /*10*/ 'Q','W','E','R','T','Y','U','I','O','P','{','}', 10, 0,'A','S',
    /*20*/ 'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    /*30*/ 'B','N','M','<','>','?', 0, '*', 0, ' ', 0,  0,  0,  0,  0,  0,
    /*40*/ 0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    /*50*/ '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /*60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /*70*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static uint16_t pit_read(void) {
    outb(0x43, 0x00);
    uint16_t lo = inb(0x40);
    uint16_t hi = inb(0x40);
    return (uint16_t)((hi << 8) | lo);
}

static void pit_init(void) {
    uint16_t div = (uint16_t)(1193182u / 100u);
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)(div >> 8));
    pit_last = pit_read();
    pit_on = 1;
}

uint32_t timer_ticks(void) { return ticks; }

static void timer_poll(void) {
    if (!pit_on) return;
    uint16_t now = pit_read();
    if (now > pit_last) ticks++;
    pit_last = now;
}

static void key_push(int code) {
    if (kq_n >= 32) return;
    kq[kq_w].code = code;
    kq[kq_w].shift = shift;
    kq[kq_w].ctrl = ctrl;
    kq[kq_w].alt = alt;
    kq_w = (kq_w + 1) & 31;
    kq_n++;
}

static void mouse_push(int type) {
    if (mq_n >= 32) return;
    mq[mq_w].type = type;
    mq[mq_w].x = mouse_x;
    mq[mq_w].y = mouse_y;
    mq_w = (mq_w + 1) & 31;
    mq_n++;
}

static void kbd_feed(uint8_t sc) {
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc == 0xE1) return;
    int release = sc & 0x80;
    sc &= 0x7F;
    if (e0) {
        e0 = 0;
        if (release) return;
        int code = 0;
        if (sc == 0x48) code = KEY_UP;
        else if (sc == 0x50) code = KEY_DOWN;
        else if (sc == 0x4B) code = KEY_LEFT;
        else if (sc == 0x4D) code = KEY_RIGHT;
        else if (sc == 0x47) code = KEY_HOME;
        else if (sc == 0x4F) code = KEY_END;
        else if (sc == 0x49) code = KEY_PGUP;
        else if (sc == 0x51) code = KEY_PGDN;
        else if (sc == 0x53) code = KEY_DEL;
        if (code) key_push(code);
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift = !release; return; }
    if (sc == 0x1D) { ctrl = !release; return; }
    if (sc == 0x38) { alt = !release; return; }
    if (sc == 0x3A) { if (!release) caps ^= 1; return; }
    if (release) return;
    if (sc >= 0x3B && sc <= 0x44) {
        key_push(KEY_F1 + (sc - 0x3B));
        return;
    }
    if (sc == 0x57) { key_push(KEY_F11); return; }
    if (sc == 0x58) { key_push(KEY_F12); return; }
    char ch = shift ? map_hi[sc] : map_lo[sc];
    if (!ch) return;
    if (caps && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    else if (caps && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    key_push((unsigned char)ch);
}

static uint8_t pkt[3];
static int pkt_i;
static int prev_btn;

static int scale_delta(int d) {
    int a = d < 0 ? -d : d;
    if (a > 10) d += d / 2;
    return d;
}

static void mouse_feed(uint8_t b) {
    if (pkt_i == 0 && !(b & 0x08)) return;
    if (pkt_i == 0 && (b & 0xC0)) return;
    pkt[pkt_i++] = b;
    if (pkt_i < 3) return;
    pkt_i = 0;
    int dx = (int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0);
    int dy = (int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0);
    mouse_x += scale_delta(dx);
    mouse_y -= scale_delta(dy);
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
    int btn = pkt[0] & 0x07;
    int changed = btn ^ prev_btn;
    prev_btn = btn;
    mouse_buttons = btn;
    if (changed & 1) mouse_push((btn & 1) ? MOUSE_LDOWN : MOUSE_LUP);
    if (changed & 2) mouse_push((btn & 2) ? MOUSE_RDOWN : MOUSE_RUP);
}

static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return 0;
    return -1;
}
static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1) return 0;
    return -1;
}
static void ps2_cmd(uint8_t c) {
    if (ps2_wait_write()) return;
    outb(0x64, c);
}
static void ps2_data(uint8_t c) {
    if (ps2_wait_write()) return;
    outb(0x60, c);
}
static int ps2_read(void) {
    if (ps2_wait_read()) return -1;
    return inb(0x60);
}
static void mouse_write(uint8_t v) {
    ps2_cmd(0xD4);
    ps2_data(v);
}
static void mouse_ack(void) {
    for (int i = 0; i < 10000; i++) {
        if (inb(0x64) & 1) {
            if (inb(0x60) == 0xFA) return;
        }
    }
}

static void mouse_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    ps2_cmd(0xA8);
    ps2_cmd(0x20);
    int cfg = ps2_read();
    if (cfg < 0) cfg = 0x45;
    cfg |= 0x02;
    cfg &= ~(1 << 5);
    cfg &= ~(1 << 4);
    ps2_cmd(0x60);
    ps2_data((uint8_t)cfg);
    mouse_write(0xF6);
    mouse_ack();
    mouse_write(0xF4);
    mouse_ack();
    mouse_x = gfx_width() / 2;
    mouse_y = gfx_height() / 2;
    prev_btn = 0;
    pkt_i = 0;
}

void input_init(void) {
    pit_init();
    mouse_init();
}

void input_poll(void) {
    for (int n = 0; n < 32; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        uint8_t d = inb(0x60);
        if (st & 0x20) mouse_feed(d);
        else kbd_feed(d);
    }
    timer_poll();
}

int input_key(KeyEvent *ev) {
    input_poll();
    if (!kq_n) return 0;
    *ev = kq[kq_r];
    kq_r = (kq_r + 1) & 31;
    kq_n--;
    return 1;
}

int input_mouse(MouseEvent *ev) {
    if (!mq_n) return 0;
    *ev = mq[mq_r];
    mq_r = (mq_r + 1) & 31;
    mq_n--;
    return 1;
}
