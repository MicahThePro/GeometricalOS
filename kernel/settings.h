#ifndef GOS_SETTINGS_H
#define GOS_SETTINGS_H

#include <stdint.h>

void settings_load(void);
void settings_save(void);

int settings_scheme(void);
void settings_set_scheme(int scheme);
uint32_t settings_desktop_color(void);
uint32_t settings_title_color(void);
uint32_t settings_highlight_color(void);

int settings_saver_on(void);
int settings_saver_seconds(void);
void settings_set_saver(int on, int seconds);

int settings_mouse_speed(void);
int settings_dbl_ticks(void);
int settings_swap_buttons(void);
void settings_set_mouse(int speed, int dbl_ticks, int swap);

int settings_sound_on(void);
int settings_volume(void);
void settings_set_sound(int on, int volume);

#endif
