#ifndef GOS_RTC_H
#define GOS_RTC_H

#include <stdint.h>

typedef struct {
    int sec, min, hour, day, month, year, wday;
} RtcTime;

void rtc_read(RtcTime *t);
int rtc_seconds_of_day(void);
void sleep_seconds(int seconds);

#endif
