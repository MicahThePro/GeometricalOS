#ifndef GOS_INPUT_H
#define GOS_INPUT_H

#include <stdint.h>

enum {
    KEY_UP = 0x80,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_DEL,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12
};

enum {
    MOUSE_LDOWN = 1,
    MOUSE_LUP,
    MOUSE_RDOWN,
    MOUSE_RUP
};

typedef struct {
    int code;
    int shift;
    int ctrl;
    int alt;
} KeyEvent;

typedef struct {
    int type;
    int x;
    int y;
} MouseEvent;

#define KBD_AA 1
#define KBD_NO_RESET 2
#define KBD_IRQ 4

void input_init(void);
void input_kbd_reinit(int flags);
int input_step_count(void);
const char *input_step_line(int index);
int input_ps2_present(void);
void input_poll(void);
void input_idle(void);
void input_post_scancode(uint8_t scancode);
void input_post_mouse(int dx, int dy, int buttons);
void input_set_pointer(int x, int y, int buttons);
void input_timer(void);
int input_key(KeyEvent *ev);
int input_mouse(MouseEvent *ev);
uint32_t timer_ticks(void);
void timer_calibrate(void);
void timer_wait_ms(int ms);
const char *timer_source(void);
int timer_pit_alive(void);
int timer_hpet_found(void);
uint32_t input_byte_count(void);
uint32_t input_ps2_kbd_bytes(void);
uint32_t input_ps2_aux_bytes(void);
uint32_t input_usb_kbd_packets(void);
uint32_t input_usb_mouse_packets(void);
int input_ps2_log_count(void);
void input_ps2_log_get(int newest, uint8_t *status, uint8_t *data);
void input_note_ps2(uint8_t status, uint8_t data);

extern int mouse_x;
extern int mouse_y;
extern int mouse_buttons;

#endif
