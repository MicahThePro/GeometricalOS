#ifndef GOS_SOUND_H
#define GOS_SOUND_H

#include <stdint.h>

void sound_startup(void);
void sound_shutdown(void);

void sound_tick(void);
int sound_play_id(int id);
void sound_stop(void);
void sound_pause(int paused);
int sound_paused(void);
int sound_playing(void);
int sound_position(void);
int sound_length(void);
const char *sound_tune_name(int id);
int sound_tune_count(void);
int sound_play_pcm(const unsigned char *pcm, int samples, int rate);
void sound_test(void);
const char *sound_detect_text(void);

#endif
