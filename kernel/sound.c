#include "sound.h"
#include "input.h"
#include "io.h"
#include "serial.h"

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

void sound_startup(void) { play_melody(0); }
void sound_shutdown(void) { play_melody(1); }
