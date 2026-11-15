#ifndef GOS_GUI_H
#define GOS_GUI_H

#include <stdint.h>

enum {
    ICON_GROUP = 1,
    ICON_FILES,
    ICON_NOTE,
    ICON_CALC,
    ICON_CLOCK,
    ICON_PAINT,
    ICON_MINE,
    ICON_TERM,
    ICON_SOL
};

enum {
    EVT_PAINT = 1,
    EVT_KEY,
    EVT_DOWN,
    EVT_UP,
    EVT_MOVE,
    EVT_RDOWN,
    EVT_MENU
};

#define MENU_MAX 4
#define ITEM_MAX 8

typedef struct Window Window;

typedef struct {
    int type;
    int x, y;
    int dbl;
    int code;
    int shift, ctrl, alt;
    int menu_id;
} Event;

typedef struct {
    char label[20];
    char items[ITEM_MAX][28];
    int ids[ITEM_MAX];
    int count;
} MenuCol;

struct Window {
    int used;
    int x, y, w, h;
    int z;
    int minimized;
    int maximized;
    int rx, ry, rw, rh;
    int keep;
    char title[48];
    MenuCol menus[MENU_MAX];
    int menu_count;
    void (*event)(Window *w, Event *e);
    Window **owner;
    int icon;
    void *user;
};

void gui_run(void);
void gui_invalidate(void);
Window *gui_create(const char *title, int w, int h, int icon,
                   void (*event)(Window *, Event *), Window **owner);
void gui_close(Window *w);
void gui_focus(Window *w);
void gui_move(Window *w, int x, int y);
void gui_restore(Window *w);
void window_client(Window *w, int *x, int *y, int *cw, int *ch);
void window_add_menu(Window *w, const char *label);
void window_add_item(Window *w, int menu, const char *label, int id);

int gui_dialog(const char *title, const char *msg, const char *b1, const char *b2, const char *b3);
int gui_prompt(const char *title, const char *label, char *buf, int buflen);
int gui_pick(const char *title, char names[][28], int count);
void gui_set_launcher(void (*fn)(int fkey));

void icons_draw(int id, int x, int y);

/* Session controls used by Program Manager. */
void session_shutdown(void);
void session_restart(void);

#endif
