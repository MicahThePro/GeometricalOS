#include "rtc.h"
#include "input.h"
#include "io.h"
#include "settings.h"

static uint8_t cmos(uint8_t reg) {
    outb(0x70, (uint8_t)(reg | 0x80)); /* keep NMI disabled */
    io_wait();
    return inb(0x71);
}

static void cmos_write(uint8_t reg, uint8_t value) {
    outb(0x70, (uint8_t)(reg | 0x80));
    io_wait();
    outb(0x71, value);
    io_wait();
}

static int from_bcd(uint8_t v, int bcd) {
    if (!bcd) return v;
    return (v & 0x0F) + ((v >> 4) * 10);
}

static uint8_t to_bcd(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && leap_year(year)) return 29;
    return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

static int weekday(int year, int month, int day) {
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year--;
    return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
}

static void shift_day(RtcTime *t, int direction) {
    if (direction > 0) {
        if (t->day < days_in_month(t->year, t->month)) t->day++;
        else {
            t->day = 1;
            if (++t->month > 12) { t->month = 1; t->year++; }
        }
    } else {
        if (t->day > 1) t->day--;
        else {
            if (--t->month < 1) { t->month = 12; t->year--; }
            t->day = days_in_month(t->year, t->month);
        }
    }
}

static void shift_minutes(RtcTime *t, int minutes) {
    int total = t->hour * 60 + t->min + minutes;
    while (total < 0) {
        total += 1440;
        shift_day(t, -1);
    }
    while (total >= 1440) {
        total -= 1440;
        shift_day(t, 1);
    }
    t->hour = total / 60;
    t->min = total % 60;
    t->wday = weekday(t->year, t->month, t->day);
}

void rtc_read(RtcTime *t) {
    if (!t) return;
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
    if (t->day > days_in_month(t->year, t->month)) t->day = 1;
    shift_minutes(t, settings_timezone_minutes());
}

int rtc_local_valid_for_offset(const RtcTime *t, int timezone_minutes) {
    if (!t || t->year < 2000 || t->year > 2099 ||
        timezone_minutes < -720 || timezone_minutes > 840 ||
        t->month < 1 || t->month > 12 ||
        t->day < 1 || t->day > days_in_month(t->year, t->month) ||
        t->hour < 0 || t->hour > 23 ||
        t->min < 0 || t->min > 59 ||
        t->sec < 0 || t->sec > 59)
        return 0;

    RtcTime utc = *t;
    shift_minutes(&utc, -timezone_minutes);
    return utc.year >= 2000 && utc.year <= 2099;
}

int rtc_set_local_with_offset(const RtcTime *t, int timezone_minutes) {
    if (!rtc_local_valid_for_offset(t, timezone_minutes)) return -1;

    RtcTime utc = *t;
    shift_minutes(&utc, -timezone_minutes);

    while (cmos(0x0A) & 0x80) {}
    uint8_t status_b = cmos(0x0B);
    int bcd = !(status_b & 0x04);
    int hour12 = !(status_b & 0x02);
    cmos_write(0x0B, (uint8_t)(status_b | 0x80));

    int hour = utc.hour;
    if (hour12) {
        int pm = hour >= 12;
        hour %= 12;
        if (!hour) hour = 12;
        if (pm) hour |= 0x80;
    }
    cmos_write(0x00, bcd ? to_bcd(utc.sec) : (uint8_t)utc.sec);
    cmos_write(0x02, bcd ? to_bcd(utc.min) : (uint8_t)utc.min);
    cmos_write(0x04, bcd ? (uint8_t)(to_bcd(hour & 0x7F) | (hour & 0x80)) : (uint8_t)hour);
    cmos_write(0x06, (uint8_t)(utc.wday + 1));
    cmos_write(0x07, bcd ? to_bcd(utc.day) : (uint8_t)utc.day);
    cmos_write(0x08, bcd ? to_bcd(utc.month) : (uint8_t)utc.month);
    cmos_write(0x09, bcd ? to_bcd(utc.year - 2000) : (uint8_t)(utc.year - 2000));
    cmos_write(0x0B, status_b);
    return 0;
}

int rtc_set_local(const RtcTime *t) {
    return rtc_set_local_with_offset(t, settings_timezone_minutes());
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
