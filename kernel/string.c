#include "io.h"

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static char lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && lower(*a) == lower(*b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

void strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++)) {}
}

void strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    if (n) dst[i] = 0;
}

#include <stdarg.h>

/* Bounded formatter: %s, %c, %u, %d and %<digits>x. Truncates at cap-1. */
int kfmt(char *dst, int cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    for (; *fmt && n + 1 < cap; fmt++) {
        if (*fmt != '%') { dst[n++] = *fmt; continue; }
        fmt++;
        int digits = 0;
        while (*fmt >= '0' && *fmt <= '9') digits = digits * 10 + *fmt++ - '0';
        char tmp[16];
        int t = 0;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "";
            while (*s && n + 1 < cap) dst[n++] = *s++;
            continue;
        } else if (*fmt == 'c') {
            dst[n++] = (char)va_arg(ap, int);
            continue;
        } else if (*fmt == 'u' || *fmt == 'd') {
            uint32_t v = va_arg(ap, uint32_t);
            int neg = 0;
            if (*fmt == 'd' && (int32_t)v < 0) { neg = 1; v = (uint32_t)-(int32_t)v; }
            if (!v) tmp[t++] = '0';
            while (v && t < 10) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            if (neg && n + 1 < cap) dst[n++] = '-';
        } else if (*fmt == 'x') {
            uint32_t v = va_arg(ap, uint32_t);
            int w = digits ? digits : 1;
            /* Low nibble first, like the decimal case: the loop below reverses it. */
            for (int s = 0; s < w * 4 && t < 16; s += 4)
                tmp[t++] = "0123456789ABCDEF"[(v >> s) & 0xF];
        } else {
            dst[n++] = *fmt;
            continue;
        }
        while (t && n + 1 < cap) dst[n++] = tmp[--t];
    }
    va_end(ap);
    if (cap > 0) dst[n < cap ? n : cap - 1] = 0;
    return n;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
