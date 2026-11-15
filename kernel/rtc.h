#ifndef GOS_RTC_H
#define GOS_RTC_H

#include <stdint.h>

typedef struct {
    int sec, min, hour, day, month, year, wday;
} RtcTime;

void rtc_read(RtcTime *t);
int rtc_local_valid_for_offset(const RtcTime *t, int timezone_minutes);
int rtc_set_local_with_offset(const RtcTime *t, int timezone_minutes);
int rtc_set_local(const RtcTime *t);
int rtc_seconds_of_day(void);
void sleep_seconds(int seconds);

#endif
