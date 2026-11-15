#include "rtc.h"
#include "input.h"
#include "io.h"

static uint8_t cmos(uint8_t reg) {
    outb(0x70, (uint8_t)(reg | 0x80)); /* keep NMI disabled */
    io_wait();
    return inb(0x71);
}

static int from_bcd(uint8_t v, int bcd) {
    if (!bcd) return v;
    return (v & 0x0F) + ((v >> 4) * 10);
}

void rtc_read(RtcTime *t) {
    for (int n = 0; n < 4; n++) {
        while (cmos(0x0A) & 0x80) {}
        uint8_t status_b = cmos(0x0B);
        int bcd = !(status_b & 0x04);
        int hour12 = !(status_b & 0x02);
        uint8_t raw_h = cmos(0x04);
        int pm = 0;
        if (hour12) {
            pm = (raw_h & 0x80) != 0;
            raw_h &= 0x7F;
        }
        t->sec = from_bcd(cmos(0x00), bcd);
        t->min = from_bcd(cmos(0x02), bcd);
        t->hour = from_bcd(raw_h, bcd);
        t->wday = from_bcd(cmos(0x06), bcd);
        t->day = from_bcd(cmos(0x07), bcd);
        t->month = from_bcd(cmos(0x08), bcd);
        t->year = from_bcd(cmos(0x09), bcd) + 2000;
        if (hour12) {
            if (t->hour == 12) t->hour = 0;
            if (pm) t->hour += 12;
        }
        if (t->sec == from_bcd(cmos(0x00), bcd)) break;
    }
    if (t->sec < 0 || t->sec > 59) t->sec = 0;
    if (t->min < 0 || t->min > 59) t->min = 0;
    if (t->hour < 0 || t->hour > 23) t->hour = 0;
    if (t->day < 1 || t->day > 31) t->day = 1;
    if (t->month < 1 || t->month > 12) t->month = 1;
    if (t->wday < 0 || t->wday > 6) t->wday = 0;
}

int rtc_seconds_of_day(void) {
    RtcTime t;
    rtc_read(&t);
    return t.hour * 3600 + t.min * 60 + t.sec;
}

void sleep_seconds(int seconds) {
    int start = rtc_seconds_of_day();
    uint32_t spins = 0;
    for (;;) {
        input_poll();
        int now = rtc_seconds_of_day();
        int d = now - start;
        if (d < 0) d += 86400;
        if (d >= seconds) return;
        if (++spins > 8000000u) return;
    }
}
