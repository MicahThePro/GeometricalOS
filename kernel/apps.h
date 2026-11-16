#ifndef GOS_APPS_H
#define GOS_APPS_H

void app_notepad_open(void);
void app_notepad_load(int inode);
void app_calc_open(void);
void app_clock_open(void);
void app_paint_open(void);
void app_paint_load(int inode);
void app_mines_open(void);
void app_files_open(void);
int app_files_open_inode(int inode);
void app_recent_add(int inode);
void app_geofind_open(void);
void app_recent_open(void);
void app_run_open(void);
void app_help_open(void);
void app_storage_open(void);
void app_term_open(void);
void app_sol_open(void);
void app_snake_open(void);
void app_write_open(void);
void app_write_load(int inode);
void app_charmap_open(void);
int app_charmap_clipboard(void);
void app_cardfile_open(void);
void app_cardfile_load(int inode);
void app_recycle_open(void);
void app_recovery_open(void);
void app_control_open(void);
void app_player_open(void);
int login_screen(void);

void desktop_init(void);
void splash_screen(void);
int run_setup(void);
void reboot(void);

#endif
