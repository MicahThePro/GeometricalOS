#ifndef GOS_SERIAL_H
#define GOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_hex32(uint32_t v);
void serial_dec(uint32_t v);

#endif
