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

static uint16_t sb_base;
static int sb_ok;
static int sb_checked;
static int sb_dma_active;
static uint32_t sb_dma_until;
#define SB16_RATE 11025u
#define SB16_DMA_ADDR 0x78000u
#define SB16_MAX_SAMPLES 16000u

static void sb_mixer(uint8_t reg, uint8_t val);

static void sb_set_volume(void) {
    int vol = settings_volume();
    if (vol < 0) vol = 0;
    if (vol > 15) vol = 15;
    uint8_t level = (uint8_t)(((vol * 31 / 15) << 3) | 0x07);
    sb_mixer(0x22, level);
    sb_mixer(0x30, level);
    sb_mixer(0x31, level);
    sb_mixer(0x32, level);
    sb_mixer(0x33, level);
}

static void sb_udelay(void) {
    io_wait();
    io_wait();
}

static int sb_write_b(uint16_t base, uint8_t v) {
    for (int i = 0; i < 4096; i++) {
        if (!(inb((uint16_t)(base + 0x0C)) & 0x80)) {
            outb((uint16_t)(base + 0x0C), v);
            return 0;
        }
    }
    return -1;
}

static int sb_write(uint8_t v) {
    if (!sb_base) return -1;
    return sb_write_b(sb_base, v);
}

static int sb_read_b(uint16_t base) {
    for (int i = 0; i < 4096; i++) {
        if (inb((uint16_t)(base + 0x0E)) & 0x80) return inb((uint16_t)(base + 0x0A));
    }
    return -1;
}

static void sb_mixer(uint8_t reg, uint8_t val) {
    outb((uint16_t)(sb_base + 4), reg);
    sb_udelay();
    outb((uint16_t)(sb_base + 5), val);
}

static int sb_reset_base(uint16_t base) {
    outb((uint16_t)(base + 6), 1);
    sb_udelay();
    sb_udelay();
    outb((uint16_t)(base + 6), 0);
    for (int i = 0; i < 20000; i++) {
        if (inb((uint16_t)(base + 0x0E)) & 0x80) {
            if (inb((uint16_t)(base + 0x0A)) == 0xAA) return 1;
        }
    }
    return 0;
}

static int sb16_present(void) {
    static const uint16_t bases[] = {0x220, 0x240, 0x260, 0x280};
    if (sb_checked) return sb_ok;
    sb_checked = 1;
    for (int i = 0; i < 4; i++) {
        if (!sb_reset_base(bases[i])) continue;
        sb_base = bases[i];
        sb_ok = 1;
        sb_write_b(sb_base, 0xE1);
        int maj = sb_read_b(sb_base);
        int min = sb_read_b(sb_base);
        sb_set_volume();
        sb_write(0xD1);
        serial_write("SOUND: sb16 at ");
        serial_hex32(sb_base);
        serial_write(" dsp ");
        serial_dec(maj < 0 ? 0 : (uint32_t)maj);
        serial_write(".");
        serial_dec(min < 0 ? 0 : (uint32_t)min);
        serial_write("\n");
        return 1;
    }
    serial_write("SOUND: no sb16\n");
    return 0;
}

const char *sound_detect_text(void) {
    sb16_present();
    if (!sb_ok) return "No Sound Blaster found. Using the PC speaker.";
    if (sb_base == 0x220) return "Sound Blaster 16 at 220h, IRQ 5, DMA 1.";
    if (sb_base == 0x240) return "Sound Blaster 16 at 240h.";
    if (sb_base == 0x260) return "Sound Blaster 16 at 260h.";
    return "Sound Blaster 16 found.";
}

static void sb16_stop(void) {
    if (sb_ok && sb_dma_active) {
        sb_write(0xD0);
        outb(0x0A, 0x05);
        (void)inb((uint16_t)(sb_base + 0x0E));
    }
    sb_dma_active = 0;
}

/* Start 8-bit single-cycle DMA on channel 1 using the fixed low-memory buffer. */
static int sb16_start(const uint8_t *src, uint32_t len) {
    if (!src || len < 2 || len > SB16_MAX_SAMPLES || !sb16_present()) return -1;
    sb16_stop();
    uint8_t *buf = (uint8_t *)SB16_DMA_ADDR;
    memcpy(buf, src, len);
    uint32_t addr = SB16_DMA_ADDR;
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

    sb_set_volume();
    if (sb_write(0xD1) ||
        sb_write(0x41) ||
        sb_write((uint8_t)(SB16_RATE >> 8)) ||
        sb_write((uint8_t)SB16_RATE) ||
        sb_write(0x40) ||
        sb_write(165) ||
        sb_write(0x14) ||
        sb_write((uint8_t)count) ||
        sb_write((uint8_t)(count >> 8))) {
        outb(0x0A, 0x05);
        serial_write("SOUND: SB16 DSP did not accept playback\n");
        return -1;
    }
    sb_dma_active = 1;
    sb_dma_until = timer_ticks() + (len * 100u + SB16_RATE - 1) / SB16_RATE + 2;
    return 0;
}

static void fill_tone(uint8_t *dst, int samples, int freq, int volume) {
    int period = (int)(SB16_RATE / (2u * (uint32_t)freq));
    if (period < 1) period = 1;
    for (int i = 0; i < samples; i++)
        dst[i] = (uint8_t)(128 + (((i / period) & 1) ? volume : -volume));
}

static void play_melody(int shutdown) {
    if (!sb16_present()) {
        serial_write("SOUND: pc-speaker\n");
        if (shutdown) {
            pc_tone(392, 10);
            pc_tone(330, 10);
            pc_tone(262, 16);
        } else {
            pc_tone(523, 8);
            pc_tone(659, 8);
            pc_tone(784, 14);
        }
        return;
    }
    uint8_t wave[6000];
    if (shutdown) {
        fill_tone(wave, 2000, 392, 40);
        fill_tone(wave + 2000, 2000, 330, 40);
        fill_tone(wave + 4000, 2000, 262, 40);
    } else {
        fill_tone(wave, 1800, 523, 50);
        fill_tone(wave + 1800, 1800, 659, 50);
        fill_tone(wave + 3600, 2400, 784, 50);
    }
    if (sb16_start(wave, 6000) < 0)
        serial_write("SOUND: SB16 playback failed\n");
    else
        serial_write("SOUND: sb16\n");
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
static uint8_t note_wave[2048];

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
        play_pos = play_len;
        sb16_stop();
        speaker_off();
        return;
    }
    int dur = tunes[play_id][play_note].dur;
    if (dur < 1) dur = 1;
    note_until = timer_ticks() + (uint32_t)dur;
    if (sb16_present()) {
        int samples = dur * (int)(SB16_RATE / 100u);
        int amplitude = settings_volume() * 6;
        fill_tone(note_wave, samples, tunes[play_id][play_note].freq, amplitude);
        if (sb16_start(note_wave, (uint32_t)samples) < 0)
            speaker_freq(tunes[play_id][play_note].freq);
    } else {
        speaker_freq(tunes[play_id][play_note].freq);
    }
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
    sb16_stop();
    speaker_off();
}

void sound_pause(int on) {
    paused = on ? 1 : 0;
    if (paused) {
        sb16_stop();
        speaker_off();
    }
    else if (active) start_note();
}

void sound_tick(void) {
    if (sb_dma_active && (int32_t)(timer_ticks() - sb_dma_until) >= 0)
        sb16_stop();
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
        pc_tone(440, 2);
        return 0;
    }
    uint32_t n = (uint32_t)samples;
    if (n > SB16_MAX_SAMPLES) n = SB16_MAX_SAMPLES;
    int vol = settings_volume();
    uint8_t scaled[16000];
    for (uint32_t i = 0; i < n; i++) {
        int s = (int)pcm[i] - 128;
        s = s * (vol + 1) / 16;
        scaled[i] = (uint8_t)(128 + s);
    }
    return sb16_start(scaled, n);
}

int sound_test(void) {
    if (!sb16_present()) {
        serial_write("SOUND: test speaker\n");
        pc_tone(880, 3);
        pc_tone(660, 3);
        return 1;
    }
    serial_write("SOUND: test sb16\n");
    uint8_t wave[4000];
    fill_tone(wave, 4000, 880, 60);
    return sb16_start(wave, 4000);
}
