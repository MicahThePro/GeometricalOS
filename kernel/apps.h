#ifndef GOS_APPS_H
#define GOS_APPS_H

void app_notepad_open(void);
void app_notepad_load(int inode);
void app_calc_open(void);
void app_clock_open(void);
void app_paint_open(void);
void app_mines_open(void);
void app_files_open(void);
void app_term_open(void);
void app_sol_open(void);
int login_screen(void);

void desktop_init(void);
void splash_screen(void);
int run_setup(void);
void reboot(void);

#endif
