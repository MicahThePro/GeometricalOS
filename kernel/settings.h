#ifndef GOS_SETTINGS_H
#define GOS_SETTINGS_H

#include <stdint.h>

void settings_load(void);
int settings_save(void);

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

int settings_timezone_minutes(void);
int settings_set_timezone_minutes(int minutes);

int settings_wallpaper(void);
void settings_set_wallpaper(int id);
int settings_icon_x(int icon);
int settings_icon_y(int icon);
void settings_set_icon_position(int icon, int x, int y);
void settings_set_icon_positions(const int x[5], const int y[5]);
void settings_enter_safe_mode(void);
int settings_safe_mode_active(void);
void settings_reset_defaults(void);
int settings_restore_backup(void);

#endif
