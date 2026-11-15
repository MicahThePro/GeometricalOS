#include "apps.h"
#include "gui.h"
#include "gfx.h"
#include "rtc.h"
#include "input.h"
#include "serial.h"

#define SNAKE_COLS 24
#define SNAKE_ROWS 14
#define SNAKE_CELL 14
#define SNAKE_CAPACITY (SNAKE_COLS * SNAKE_ROWS)
#define SNAKE_TICKS 12u

enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

static Window *win;
static uint8_t body_x[SNAKE_CAPACITY];
static uint8_t body_y[SNAKE_CAPACITY];
static int length;
static int direction;
static int next_direction;
static int food_x, food_y;
static int score, best_score;
static int paused, dead, won;
static uint32_t rng = 1;

static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static uint32_t game_delay(void) {
    uint32_t delay = SNAKE_TICKS - (uint32_t)(score / 50);
    return delay < 5 ? 5 : delay;
}

static int occupied(int x, int y) {
    for (int i = 0; i < length; i++)
        if (body_x[i] == x && body_y[i] == y) return 1;
    return 0;
}

static int place_food(void) {
    int total = SNAKE_CAPACITY;
    int start = (int)(rnd() % (uint32_t)total);
    for (int i = 0; i < total; i++) {
        int cell = (start + i) % total;
        int x = cell % SNAKE_COLS;
        int y = cell / SNAKE_COLS;
        if (!occupied(x, y)) {
            food_x = x;
            food_y = y;
            return 1;
        }
    }
    return 0;
}

static void reset_game(void) {
    length = 4;
    for (int i = 0; i < length; i++) {
        body_x[i] = (uint8_t)(SNAKE_COLS / 2 - i);
        body_y[i] = (uint8_t)(SNAKE_ROWS / 2);
    }
    direction = DIR_RIGHT;
    next_direction = DIR_RIGHT;
    score = 0;
    paused = dead = won = 0;
    rng ^= timer_ticks() + (uint32_t)rtc_seconds_of_day() * 2654435761u;
    rng ^= (uint32_t)(mouse_x + mouse_y) | 1u;
    place_food();
    if (win) gui_set_timer(win, SNAKE_TICKS);
}

static void set_direction(int next) {
    if ((direction + 2) % 4 == next) return;
    next_direction = next;
}

static void step_game(void) {
    direction = next_direction;
    int x = body_x[0];
    int y = body_y[0];
    if (direction == DIR_UP) y--;
    else if (direction == DIR_RIGHT) x++;
    else if (direction == DIR_DOWN) y++;
    else x--;

    if (x < 0 || y < 0 || x >= SNAKE_COLS || y >= SNAKE_ROWS) {
        dead = 1;
        gui_set_timer(win, 0);
        return;
    }

    int eat = x == food_x && y == food_y;
    int check = eat ? length : length - 1;
    for (int i = 0; i < check; i++) {
        if (body_x[i] == x && body_y[i] == y) {
            dead = 1;
            gui_set_timer(win, 0);
            return;
        }
    }

    if (eat) {
        length++;
        score += 10;
        if (score > best_score) best_score = score;
    }
    for (int i = length - 1; i > 0; i--) {
        body_x[i] = body_x[i - 1];
        body_y[i] = body_y[i - 1];
    }
    body_x[0] = (uint8_t)x;
    body_y[0] = (uint8_t)y;

    if (eat) {
        if (length == SNAKE_CAPACITY || !place_food()) {
            won = 1;
            gui_set_timer(win, 0);
            return;
        }
    }
    gui_set_timer(win, game_delay());
}

static void draw_number(int x, int y, const char *label, int value) {
    char digits[12];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0 && n < (int)sizeof(digits) - 1) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    digits[n] = 0;
    for (int i = 0; i < n / 2; i++) {
        char t = digits[i];
        digits[i] = digits[n - i - 1];
        digits[n - i - 1] = t;
    }
    gfx_text(x, y, label, COL_BLACK, COL_GRAY);
    gfx_text(x + gfx_text_width(label), y, digits, COL_BLACK, COL_GRAY);
}

static void snake_event(Window *w, Event *e) {
    int cx, cy, cw, ch;
    window_client(w, &cx, &cy, &cw, &ch);
    int board_w = SNAKE_COLS * SNAKE_CELL;
    int board_h = SNAKE_ROWS * SNAKE_CELL;
    int board_x = (cw - board_w) / 2;
    int board_y = 40;
    int new_x = cw - 198;
    int pause_x = cw - 102;

    if (e->type == EVT_PAINT) {
        gfx_fill(cx, cy, cw, ch, COL_GRAY);
        draw_number(cx + 10, cy + 10, "Score: ", score);
        draw_number(cx + 94, cy + 10, "Best: ", best_score);
        gfx_button(cx + new_x, cy + 6, 88, 24, "New Game", 0);
        gfx_button(cx + pause_x, cy + 6, 92, 24, paused ? "Resume" : "Pause", 0);

        gfx_sunken(cx + board_x - 3, cy + board_y - 3,
                   board_w + 6, board_h + 6, COL_GRAY);
        gfx_fill(cx + board_x, cy + board_y, board_w, board_h, 0x003820);
        for (int x = 0; x <= SNAKE_COLS; x++)
            gfx_vline(cx + board_x + x * SNAKE_CELL, cy + board_y, board_h, 0x004A2C);
        for (int y = 0; y <= SNAKE_ROWS; y++)
            gfx_hline(cx + board_x, cy + board_y + y * SNAKE_CELL, board_w, 0x004A2C);

        gfx_fill_circle(cx + board_x + food_x * SNAKE_CELL + SNAKE_CELL / 2,
                        cy + board_y + food_y * SNAKE_CELL + SNAKE_CELL / 2,
                        5, COL_RED);
        gfx_vline(cx + board_x + food_x * SNAKE_CELL + SNAKE_CELL / 2,
                  cy + board_y + food_y * SNAKE_CELL + 1, 4, COL_GREEN);
        for (int i = length - 1; i >= 0; i--) {
            int px = cx + board_x + body_x[i] * SNAKE_CELL + 1;
            int py = cy + board_y + body_y[i] * SNAKE_CELL + 1;
            uint32_t color = i == 0 ? 0x80FF80 : 0x00C040;
            gfx_fill(px, py, SNAKE_CELL - 2, SNAKE_CELL - 2, color);
            gfx_rect(px, py, SNAKE_CELL - 2, SNAKE_CELL - 2, 0x001C10);
        }
        if (paused) gfx_text(cx + board_x + board_w / 2 - 24,
                             cy + board_y + board_h / 2 - 4, "PAUSED", COL_WHITE, 0x003820);
        else if (dead) gfx_text(cx + board_x + board_w / 2 - 40,
                                cy + board_y + board_h / 2 - 4, "GAME OVER", COL_WHITE, 0x003820);
        else if (won) gfx_text(cx + board_x + board_w / 2 - 16,
                               cy + board_y + board_h / 2 - 4, "YOU WIN", COL_WHITE, 0x003820);
        gfx_text(cx + 10, cy + board_y + board_h + 10,
                 "Arrows/WASD move   Space pause   N new game", COL_BLACK, COL_GRAY);
        return;
    }

    if (e->type == EVT_TIMER) {
        if (!paused && !dead && !won) step_game();
        return;
    }

    if (e->type == EVT_DOWN) {
        if (e->y >= 6 && e->y < 30 && e->x >= new_x && e->x < new_x + 88) {
            reset_game();
            return;
        }
        if (e->y >= 6 && e->y < 30 && e->x >= pause_x && e->x < pause_x + 92) {
            if (dead || won) return;
            paused = !paused;
            gui_set_timer(win, paused ? 0 : game_delay());
            return;
        }
    }

    if (e->type != EVT_KEY) return;
    if (e->code == 'n' || e->code == 'N') {
        reset_game();
    } else if (e->code == ' ' && !dead && !won) {
        paused = !paused;
        gui_set_timer(win, paused ? 0 : game_delay());
    } else if (e->code == KEY_UP || e->code == 'w' || e->code == 'W') {
        set_direction(DIR_UP);
    } else if (e->code == KEY_RIGHT || e->code == 'd' || e->code == 'D') {
        set_direction(DIR_RIGHT);
    } else if (e->code == KEY_DOWN || e->code == 's' || e->code == 'S') {
        set_direction(DIR_DOWN);
    } else if (e->code == KEY_LEFT || e->code == 'a' || e->code == 'A') {
        set_direction(DIR_LEFT);
    }
}

void app_snake_open(void) {
    if (win) {
        gui_restore(win);
        return;
    }
    win = gui_create("Snake", 430, 340, ICON_SNAKE, snake_event, &win);
    if (!win) return;
    win->text_keys = 1;
    reset_game();
    serial_write("APP: SNAKE\n");
}
