#include "sound.h"
#include "input.h"
#include "io.h"
#include "serial.h"
#include "settings.h"

/* PC speaker plus Sound Blaster 16 direct playback.
   VirtualBox: set Audio controller to Sound Blaster 16 to hear the DSP
   chime. The PC speaker still plays, which QEMU mixes by default. */

static void delay_ticks(int ticks) {
    uint32_t start = timer_ticks();
    uint32_t spins = 0;
    while ((int)(timer_ticks() - start) < ticks) {
        input_poll();
        if (++spins > 2000000u) break;
    }
}

static void pc_tone(int freq, int ticks) {
    if (freq < 20) freq = 20;
    uint32_t div = 1193182u / (uint32_t)freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, (uint8_t)(inb(0x61) | 0x03));
    delay_ticks(ticks);
    outb(0x61, (uint8_t)(inb(0x61) & ~0x03));
}

static int sb_write(uint8_t v) {
    for (int i = 0; i < 80000; i++) {
        if (!(inb(0x22C) & 0x80)) {
            outb(0x22C, v);
            return 0;
        }
    }
    return -1;
}

static int sb16_present(void) {
    outb(0x226, 1);
    for (int i = 0; i < 1000; i++) inb(0x226);
    outb(0x226, 0);
    for (int i = 0; i < 80000; i++) {
        if (inb(0x22E) & 0x80) {
            if (inb(0x22A) == 0xAA) return 1;
        }
    }
    return 0;
}

/* 8-bit single-cycle DMA on channel 1, the SB16 default. */
static void sb16_play(const uint8_t *src, uint32_t len) {
    if (len < 2 || len > 16000) return;
    uint8_t *buf = (uint8_t *)0x70000;
    memcpy(buf, src, len);
    uint32_t addr = 0x70000;
    uint16_t count = (uint16_t)(len - 1);

    outb(0x0A, 0x05);
    outb(0x0C, 0x00);
    outb(0x0B, 0x49);
    outb(0x02, (uint8_t)addr);
    outb(0x02, (uint8_t)(addr >> 8));
    outb(0x83, (uint8_t)(addr >> 16));
    outb(0x03, (uint8_t)count);
    outb(0x03, (uint8_t)(count >> 8));
    outb(0x0A, 0x01);

    sb_write(0xD1);
    {
        int vol = settings_volume();
        if (vol < 0) vol = 0;
        if (vol > 15) vol = 15;
        uint8_t v = (uint8_t)((vol << 4) | vol);
        outb(0x224, 0x22);
        outb(0x225, v);
    }
    sb_write(0x40);
    sb_write(165); /* about 11025 Hz: 256 - 1000000/11025 */
    sb_write(0x14);
    sb_write((uint8_t)count);
    sb_write((uint8_t)(count >> 8));
}

static void fill_tone(uint8_t *dst, int samples, int period, int volume) {
    for (int i = 0; i < samples; i++)
        dst[i] = (uint8_t)(128 + (((i / period) & 1) ? volume : -volume));
}

static void play_melody(int shutdown) {
    /* Speaker tones. ticks are ~10 ms at the 100 Hz timer. */
    if (shutdown) {
        pc_tone(392, 10);
        pc_tone(330, 10);
        pc_tone(262, 16);
    } else {
        pc_tone(523, 8);
        pc_tone(659, 8);
        pc_tone(784, 14);
    }
    if (!sb16_present()) {
        serial_write("SOUND: pc-speaker\n");
        return;
    }
    uint8_t wave[6000];
    if (shutdown) {
        fill_tone(wave, 2000, 28, 40);
        fill_tone(wave + 2000, 2000, 33, 40);
        fill_tone(wave + 4000, 2000, 42, 40);
    } else {
        fill_tone(wave, 1800, 21, 50);
        fill_tone(wave + 1800, 1800, 17, 50);
        fill_tone(wave + 3600, 2400, 14, 50);
    }
    sb16_play(wave, 6000);
    serial_write("SOUND: sb16\n");
    delay_ticks(40);
}

void sound_startup(void) {
    if (!settings_sound_on()) return;
    play_melody(0);
}
void sound_shutdown(void) {
    if (!settings_sound_on()) return;
    play_melody(1);
}

static const struct { uint16_t freq; uint8_t dur; } tunes[][16] = {
    { {523,8},{659,8},{784,14},{0,0} },
    { {392,10},{330,10},{262,16},{0,0} },
    { {262,8},{294,8},{330,8},{349,8},{392,8},{440,8},{494,8},{523,16},{0,0} },
    { {392,6},{440,6},{494,6},{523,10},{494,6},{440,6},{392,12},{0,0} },
    { {330,8},{392,8},{494,12},{392,8},{330,16},{0,0} },
};
static const char *tune_names[] = {
    "Startup chime", "Shutdown chime", "Scale", "Fanfare", "Little march"
};
#define TUNE_COUNT 5

static int play_id = -1;
static int play_note;
static int play_pos;
static int play_len;
static int paused;
static int active;
static uint32_t note_until;

static int tune_len(int id) {
    int n = 0;
    while (n < 16 && tunes[id][n].freq) n++;
    return n;
}

const char *sound_tune_name(int id) {
    if (id < 0 || id >= TUNE_COUNT) return "";
    return tune_names[id];
}
int sound_tune_count(void) { return TUNE_COUNT; }
int sound_playing(void) { return active; }
int sound_paused(void) { return paused; }
int sound_position(void) { return play_pos; }
int sound_length(void) { return play_len; }

static void speaker_off(void) {
    outb(0x61, (uint8_t)(inb(0x61) & ~0x03));
}

static void speaker_freq(int freq) {
    if (freq < 20) { speaker_off(); return; }
    uint32_t div = 1193182u / (uint32_t)freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)div);
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, (uint8_t)(inb(0x61) | 0x03));
}

static void start_note(void) {
    if (play_id < 0 || play_note >= play_len) {
        active = 0;
        speaker_off();
        return;
    }
    int dur = tunes[play_id][play_note].dur;
    if (dur < 1) dur = 1;
    note_until = timer_ticks() + (uint32_t)dur;
    speaker_freq(tunes[play_id][play_note].freq);
}

int sound_play_id(int id) {
    if (id < 0 || id >= TUNE_COUNT) return -1;
    if (!settings_sound_on()) return -1;
    play_id = id;
    play_len = tune_len(id);
    play_note = 0;
    play_pos = 0;
    paused = 0;
    active = 1;
    start_note();
    return 0;
}

void sound_stop(void) {
    active = 0;
    paused = 0;
    play_id = -1;
    speaker_off();
}

void sound_pause(int on) {
    paused = on ? 1 : 0;
    if (paused) speaker_off();
    else if (active) start_note();
}

void sound_tick(void) {
    if (!active || paused) return;
    if ((int)(timer_ticks() - note_until) < 0) return;
    play_note++;
    play_pos = play_note;
    start_note();
}

int sound_play_pcm(const unsigned char *pcm, int samples, int rate) {
    (void)rate;
    if (!pcm || samples < 2 || !settings_sound_on()) return -1;
    if (!sb16_present()) {
        pc_tone(440, 8);
        return 0;
    }
    uint32_t n = (uint32_t)samples;
    if (n > 16000) n = 16000;
    int vol = settings_volume();
    uint8_t scaled[16000];
    for (uint32_t i = 0; i < n; i++) {
        int s = (int)pcm[i] - 128;
        s = s * (vol + 1) / 16;
        scaled[i] = (uint8_t)(128 + s);
    }
    sb16_play(scaled, n);
    return 0;
}
