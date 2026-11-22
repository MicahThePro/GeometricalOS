#include "input.h"
#include "hpet.h"
#include "gfx.h"
#include "io.h"
#include "vmm.h"
#include "serial.h"
#include "settings.h"
#include "sound.h"
#include "xhci.h"

int mouse_x, mouse_y, mouse_buttons;

static int shift, ctrl, alt, caps, e0;
static uint32_t ticks;
static uint16_t pit_last;
static uint16_t pit_div;
static uint32_t pit_acc;
static int pit_on;
static int pit_alive;
static uint32_t tsc_per_ms;
static const char *clock_source = "none";
static int hpet_ok;

static uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

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
    pit_div = (uint16_t)(1193182u / 100u);
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(pit_div & 0xFF));
    outb(0x40, (uint8_t)(pit_div >> 8));
    pit_last = pit_read();
    pit_acc = 0;
    pit_on = 1;
}

uint32_t timer_ticks(void) { return ticks; }

const char *timer_source(void) { return clock_source; }
int timer_pit_alive(void) { return pit_alive; }

void timer_wait_ms(int ms) {
    if (ms < 1) ms = 1;
    if (tsc_per_ms) {
        uint64_t end = rdtsc_now() + (uint64_t)ms * tsc_per_ms;
        while (rdtsc_now() < end) {}
        return;
    }
    if (hpet_present() && hpet_period()) {
        uint64_t need = hpet_ticks_for(hpet_period(), (uint32_t)ms);
        uint64_t start = hpet_counter();
        if (need) {
            while (hpet_counter() - start < need) {}
            return;
        }
    }
    for (int i = 0; i < ms * 200000; i++)
        __asm__ volatile ("pause");
}

void timer_calibrate(void) {
    pit_init();
    uint16_t first = pit_read();
    int moved = 0;
    for (int i = 0; i < 200000; i++) {
        if (pit_read() != first) { moved = 1; break; }
    }
    pit_alive = moved;
    if (moved) {
        uint64_t t0 = rdtsc_now();
        int wraps = 0;
        uint16_t prev = pit_read();
        for (int i = 0; i < 20000000 && wraps < 10; i++) {
            uint16_t now = pit_read();
            if (now > prev) wraps++;
            prev = now;
        }
        uint64_t t1 = rdtsc_now();
        if (wraps >= 10 && t1 > t0) {
            tsc_per_ms = (uint32_t)((t1 - t0) / (uint64_t)(wraps * 10));
            clock_source = "TSC";
        }
    }
    int hpet = hpet_present();
    hpet_ok = hpet;
    if (!tsc_per_ms && hpet && hpet_period()) {
        uint64_t need = hpet_ticks_for(hpet_period(), 20);
        uint64_t c0 = hpet_counter();
        uint64_t t0 = rdtsc_now();
        if (need) {
            while (hpet_counter() - c0 < need) {}
            uint64_t t1 = rdtsc_now();
            if (t1 > t0) {
                tsc_per_ms = (uint32_t)((t1 - t0) / 20u);
                clock_source = "HPET";
                pit_alive = 0;
            }
        }
    }
    (void)hpet;
    serial_write("CLK ");
    serial_write(clock_source);
    serial_write(pit_alive ? " pit Y" : " pit N");
    serial_write(hpet_ok ? " hpet Y\n" : " hpet N\n");
}

int timer_hpet_found(void) { return hpet_ok; }

void input_timer(void) {
    if (!pit_on || !pit_div) return;
    uint16_t now = pit_read();
    uint32_t step = pit_last >= now ? (uint32_t)(pit_last - now)
                                    : (uint32_t)pit_last + pit_div - now;
    if (step > pit_div) step = pit_div;
    pit_last = now;
    pit_acc += step;
    while (pit_acc >= pit_div) {
        pit_acc -= pit_div;
        ticks++;
    }
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

static uint32_t byte_count;
uint32_t input_byte_count(void) { return byte_count; }

#define PS2_LOG 48
static uint8_t ps2_log_st[PS2_LOG];
static uint8_t ps2_log_dt[PS2_LOG];
static int ps2_log_n, ps2_log_w;
static uint32_t ps2_kbd_bytes, ps2_aux_bytes, usb_kbd_packets, usb_mouse_packets;

uint32_t input_ps2_kbd_bytes(void) { return ps2_kbd_bytes; }
uint32_t input_ps2_aux_bytes(void) { return ps2_aux_bytes; }
uint32_t input_usb_kbd_packets(void) { return usb_kbd_packets; }
uint32_t input_usb_mouse_packets(void) { return usb_mouse_packets; }
int input_ps2_log_count(void) { return ps2_log_n; }

void input_note_ps2(uint8_t status, uint8_t data) {
    ps2_log_st[ps2_log_w] = status;
    ps2_log_dt[ps2_log_w] = data;
    ps2_log_w = (ps2_log_w + 1) % PS2_LOG;
    if (ps2_log_n < PS2_LOG) ps2_log_n++;
    if (status & 0x20) ps2_aux_bytes++;
    else ps2_kbd_bytes++;
}

void input_ps2_log_get(int newest, uint8_t *status, uint8_t *data) {
    int i = ps2_log_w - 1 - newest;
    while (i < 0) i += PS2_LOG;
    if (status) *status = ps2_log_st[i];
    if (data) *data = ps2_log_dt[i];
}

static uint8_t set2_to_set1(uint8_t c) {
    switch (c) {
    case 0x1C: return 0x1E; case 0x32: return 0x30; case 0x21: return 0x2E;
    case 0x23: return 0x20; case 0x24: return 0x12; case 0x2B: return 0x21;
    case 0x34: return 0x22; case 0x33: return 0x23; case 0x43: return 0x17;
    case 0x3B: return 0x24; case 0x42: return 0x25; case 0x4B: return 0x26;
    case 0x3A: return 0x32; case 0x31: return 0x31; case 0x44: return 0x18;
    case 0x4D: return 0x19; case 0x15: return 0x10; case 0x2D: return 0x13;
    case 0x1B: return 0x1F; case 0x2C: return 0x14; case 0x3C: return 0x16;
    case 0x2A: return 0x2F; case 0x1D: return 0x11; case 0x22: return 0x2D;
    case 0x35: return 0x15; case 0x1A: return 0x2C;
    case 0x16: return 0x02; case 0x1E: return 0x03; case 0x26: return 0x04;
    case 0x25: return 0x05; case 0x2E: return 0x06; case 0x36: return 0x07;
    case 0x3D: return 0x08; case 0x3E: return 0x09; case 0x46: return 0x0A;
    case 0x45: return 0x0B; case 0x5A: return 0x1C; case 0x76: return 0x01;
    case 0x66: return 0x0E; case 0x0D: return 0x0F; case 0x29: return 0x39;
    case 0x12: return 0x2A; case 0x59: return 0x36; case 0x14: return 0x1D;
    case 0x11: return 0x38;
    default: return 0;
    }
}

static void kbd_feed(uint8_t sc) {
    static int set2, f0;
    if (sc == 0xF0) { f0 = 1; set2 = 1; return; }
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc == 0xE1) return;
    if (set2 && !e0) {
        int release = f0;
        f0 = 0;
        uint8_t s1 = set2_to_set1(sc);
        if (!s1) return;
        sc = (uint8_t)(s1 | (release ? 0x80 : 0));
    }
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
static uint32_t pkt_tick;

static int scale_delta(int d) {
    int speed = settings_mouse_speed();
    if (speed < 1) speed = 1;
    d = d * speed / 2;
    int a = d < 0 ? -d : d;
    if (a > 8) d += d / 2;
    return d;
}

static void mouse_clamp(void) {
    int w = gfx_width();
    int h = gfx_height();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= w) mouse_x = w - 1;
    if (mouse_y >= h) mouse_y = h - 1;
}

/* Byte 0 of a PS/2 packet always has bit 3 set, and bits 6 and 7 clear
 * unless that axis overflowed. A movement byte can look similar, so a
 * header is only trusted at the start of a packet or after a gap.
 */
static int mouse_header(uint8_t b) {
    if ((b & 0x08) == 0) return 0;
    if (b & 0xC0) return 0;
    return 1;
}

static void mouse_apply(void) {
    /* Sign bits in byte 0 must agree with the high bit of each delta.
     * A mismatch means the three bytes are not one packet.
     */
    if (((pkt[0] >> 4) & 1) != (pkt[1] >> 7)) return;
    if (((pkt[0] >> 5) & 1) != (pkt[2] >> 7)) return;
    int dx = (int)(int8_t)pkt[1];
    int dy = (int)(int8_t)pkt[2];
    if (dx > 40) dx = 40;
    if (dx < -40) dx = -40;
    if (dy > 40) dy = 40;
    if (dy < -40) dy = -40;
    if (!vmm_mouse_absolute()) {
        mouse_x += scale_delta(dx);
        mouse_y -= scale_delta(dy);
        mouse_clamp();
    }
    int btn = pkt[0] & 0x07;
    if (settings_swap_buttons()) btn = (btn & ~3) | ((btn & 1) << 1) | ((btn & 2) >> 1);
    int changed = btn ^ prev_btn;
    prev_btn = btn;
    mouse_buttons = btn;
    if (changed & 1) mouse_push((btn & 1) ? MOUSE_LDOWN : MOUSE_LUP);
    if (changed & 2) mouse_push((btn & 2) ? MOUSE_RDOWN : MOUSE_RUP);
}

static void mouse_feed(uint8_t b) {
    if (pkt_i && timer_ticks() - pkt_tick > 4) pkt_i = 0;
    if (pkt_i != 0 && mouse_header(b) && timer_ticks() - pkt_tick > 1) {
        pkt_i = 0;
    }
    if (pkt_i == 0) {
        if (!mouse_header(b)) return;
        pkt[0] = b;
        pkt_i = 1;
        pkt_tick = timer_ticks();
        return;
    }
    pkt[pkt_i++] = b;
    pkt_tick = timer_ticks();
    if (pkt_i < 3) return;
    pkt_i = 0;
    mouse_apply();
}

static int ps2_wait_in(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return 0;
    return -1;
}

static int ps2_wait_byte(int spins) {
    for (int i = 0; i < spins; i++) {
        uint8_t st = inb(0x64);
        if (st & 1) {
            uint8_t d = inb(0x60);
            if (st & 0x20) {
                mouse_feed(d);
                continue;
            }
            return d;
        }
    }
    return -1;
}

static void ps2_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 1)) return;
        inb(0x60);
    }
}

static void ps2_cmd(uint8_t c) {
    if (ps2_wait_in()) return;
    outb(0x64, c);
}

static void ps2_kbd(uint8_t c) {
    if (ps2_wait_in()) return;
    outb(0x60, c);
}

static int ps2_collect(uint8_t a, uint8_t b, int reads, int spins) {
    int hit = 0;
    for (int n = 0; n < reads; n++) {
        int v = ps2_wait_byte(spins);
        if (v < 0) break;
        if (v == (int)a || v == (int)b) hit = 1;
        if (v == (int)b) break;
    }
    return hit;
}

static int ps2_acked;
static uint32_t ps2_retry_at;
static int ps2_retries;

/* The firmware is already translating keystrokes. Disabling the port or
 * resetting it stops that stream, so only ask the keyboard to scan. */
static void ps2_bringup(void) {
    int floating = 0;
    for (int i = 0; i < 16; i++) if (inb(0x64) == 0xFF) floating++;
    mouse_x = gfx_width() / 2;
    mouse_y = gfx_height() / 2;
    prev_btn = 0;
    pkt_i = 0;
    pkt_tick = 0;
    ps2_retry_at = 200;
    ps2_retries = 0;
    if (floating > 12) {
        serial_write("KBD: no controller\n");
        return;
    }
    ps2_kbd(0xF4);
    ps2_acked = ps2_collect(0xFA, 0xFA, 4, 200000);
    ps2_cmd(0xD4);
    ps2_kbd(0xF4);
    serial_write(ps2_acked ? "KBD: scanning\n" : "KBD: no ack\n");
}

static void ps2_drain(void) {
    for (int n = 0; n < 16; n++) {
        uint8_t st = inb(0x64);
        if (st == 0xFF || !(st & 1)) break;
        uint8_t d = inb(0x60);
        input_note_ps2(st, d);
        /* 0xFA and 0xAA are command replies, not keys. */
        if (d != 0xFA && d != 0xAA && d != 0xFE)
            byte_count++;
        if (st & 0x20) mouse_feed(d);
        else kbd_feed(d);
    }
}

void input_idle(void) {
    ps2_drain();
    input_timer();
}

void input_init(void) {
    pit_init();
    ps2_bringup();
    xhci_init();
    if (ps2_acked) xhci_append_status(" ps2");
}

void input_post_mouse(int dx, int dy, int buttons) {
    usb_mouse_packets++;
    if (dx > 40) dx = 40;
    if (dx < -40) dx = -40;
    if (dy > 40) dy = 40;
    if (dy < -40) dy = -40;
    if (!vmm_mouse_absolute()) {
        mouse_x += scale_delta(dx);
        mouse_y += scale_delta(dy);
        mouse_clamp();
    }
    int btn = buttons & 3;
    if (settings_swap_buttons()) btn = (btn & ~3) | ((btn & 1) << 1) | ((btn & 2) >> 1);
    int changed = btn ^ prev_btn;
    prev_btn = btn;
    mouse_buttons = btn;
    if (changed & 1) mouse_push((btn & 1) ? MOUSE_LDOWN : MOUSE_LUP);
    if (changed & 2) mouse_push((btn & 2) ? MOUSE_RDOWN : MOUSE_RUP);
}

void input_post_scancode(uint8_t scancode) {
    usb_kbd_packets++;
    byte_count++;
    kbd_feed(scancode);
}

void input_poll(void) {
    ps2_drain();
    /* Some HP keyboards answer only after the embedded controller finishes
     * starting. Ask again until a real byte shows up. */
    if (!byte_count && ps2_retries < 4 && timer_ticks() >= ps2_retry_at) {
        ps2_retries++;
        ps2_retry_at = timer_ticks() + 300;
        ps2_kbd(0xF4);
    }
    xhci_poll();
    ps2_drain();
    if (vmm_display_poll())
        mouse_clamp();
    if (vmm_mouse_poll(&mouse_x, &mouse_y, gfx_width(), gfx_height()))
        mouse_clamp();
    input_timer();
    sound_tick();
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
