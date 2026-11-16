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
    ICON_SOL,
    ICON_SNAKE,
    ICON_WRITE,
    ICON_CHARMAP,
    ICON_CARDFILE,
    ICON_RECYCLE,
    ICON_SYSTEM,
    ICON_CONTROL,
    ICON_MEDIA,
    ICON_GEOFIND,
    ICON_RECENT,
    ICON_RUN,
    ICON_HELP,
    ICON_STORAGE,
    ICON_WALLPAPER,
    ICON_MOUSE
};

enum {
    EVT_PAINT = 1,
    EVT_KEY,
    EVT_DOWN,
    EVT_UP,
    EVT_MOVE,
    EVT_RDOWN,
    EVT_MENU,
    EVT_TIMER
};

#define MENU_MAX 4
#define ITEM_MAX 8
#define GUI_TASKBAR_HEIGHT 34

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
    /* Return 0 to keep the window open (for example after Cancel). */
    int (*ask_close)(Window *w);
    int text_keys;
    Window **owner;
    int icon;
    void *user;
    uint32_t timer_interval;
    uint32_t timer_last;
};

void gui_run(void);
void gui_invalidate(void);
void gui_screen_resized(void);
Window *gui_create(const char *title, int w, int h, int icon,
                   void (*event)(Window *, Event *), Window **owner);
void gui_close(Window *w);
void gui_focus(Window *w);
void gui_move(Window *w, int x, int y);
void gui_restore(Window *w);
void gui_set_timer(Window *w, uint32_t interval);
void window_client(Window *w, int *x, int *y, int *cw, int *ch);
void window_add_menu(Window *w, const char *label);
void window_add_item(Window *w, int menu, const char *label, int id);

int gui_dialog(const char *title, const char *msg, const char *b1, const char *b2, const char *b3);
int gui_prompt(const char *title, const char *label, char *buf, int buflen);
int gui_pick(const char *title, char names[][28], int count);
/* save=1 is Save As, save=0 is Open. dir and name are updated on OK. */
int gui_file(const char *title, int save, uint16_t *dir, char *name, int namemax);
void gui_set_launcher(void (*fn)(int fkey));
void gui_set_start_handler(void (*fn)(void));
void gui_set_desktop_handler(void (*draw)(void), void (*event)(Event *));
void gui_focus_desktop(void);
int gui_desktop_focused(void);

void icons_draw(int id, int x, int y);

/* Session controls exposed through GeoDeck. */
void session_shutdown(void);
void session_restart(void);

#endif
