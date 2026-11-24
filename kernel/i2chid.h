#ifndef GOS_I2CHID_H
#define GOS_I2CHID_H

#include <stdint.h>

/* I2C-HID touchpad (ACPI PNP0C50) on an AMD DesignWare I2C controller. */
void i2chid_init(void);
void i2chid_poll(void);
int i2chid_line_count(void);
const char *i2chid_line(int index);
uint32_t i2chid_packets(void);

#endif
