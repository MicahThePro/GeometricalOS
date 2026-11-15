#include "serial.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20) break;
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

void serial_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 7; i >= 0; i--) serial_putc(hex[(v >> (i * 4)) & 0xF]);
}

void serial_dec(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) serial_putc(buf[i]);
}
