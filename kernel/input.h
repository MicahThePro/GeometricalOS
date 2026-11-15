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

void input_init(void);
void input_poll(void);
int input_key(KeyEvent *ev);
int input_mouse(MouseEvent *ev);
uint32_t timer_ticks(void);

extern int mouse_x;
extern int mouse_y;
extern int mouse_buttons;

#endif
