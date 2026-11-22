#ifndef GOS_HPET_H
#define GOS_HPET_H

#include <stdint.h>

int hpet_present(void);
uint32_t hpet_period(void);
uint64_t hpet_ticks_for(uint64_t period_fs, uint32_t ms);
uint64_t hpet_counter(void);
int hpet_prepare(void);

#endif
