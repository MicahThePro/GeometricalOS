#include "i2cscan.h"
#include "acpi.h"
#include "io.h"

/* Walk the DSDT and SSDTs for the touchpad and its I2C controller. */

static uint32_t pkg_len(const uint8_t *p, uint32_t *nbytes) {
    uint8_t lead = p[0];
    uint32_t n = lead >> 6;
    *nbytes = n + 1;
    if (!n) return lead & 0x3Fu;
    uint32_t v = lead & 0x0Fu;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    return v;
}

static int seg_ok(const uint8_t *p) {
    for (int i = 0; i < 4; i++) {
        uint8_t c = p[i];
        int ok = (c >= 'A' && c <= 'Z') || c == '_' || (i > 0 && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

static int name_string(const uint8_t *p, uint32_t max, const uint8_t **last, uint32_t *len) {
    uint32_t i = 0;
    while (i < max && (p[i] == '\\' || p[i] == '^')) i++;
    if (i >= max) return 0;
    uint32_t segs = 1;
    if (p[i] == 0x2E) {
        segs = 2;
        i++;
    } else if (p[i] == 0x2F) {
        if (i + 1 >= max) return 0;
        segs = p[i + 1];
        i += 2;
    }
    if (segs < 1 || segs > 16 || i + segs * 4 > max) return 0;
    for (uint32_t s = 0; s < segs; s++)
        if (!seg_ok(p + i + s * 4)) return 0;
    *last = p + i + (segs - 1) * 4;
    *len = i + segs * 4;
    return 1;
}

static int device_at(const uint8_t *a, uint32_t n, uint32_t i, uint32_t *body, uint32_t *end, char *name) {
    if (i + 8 > n || a[i] != 0x5B || a[i + 1] != 0x82) return 0;
    uint32_t nb;
    uint32_t len = pkg_len(a + i + 2, &nb);
    uint32_t e = i + 2 + len;
    if (len < nb + 4 || e > n) return 0;
    const uint8_t *last;
    uint32_t nl;
    if (!name_string(a + i + 2 + nb, e - (i + 2 + nb), &last, &nl)) return 0;
    *body = i + 2 + nb + nl;
    *end = e;
    for (int k = 0; k < 4; k++) name[k] = (char)last[k];
    name[4] = 0;
    return 1;
}

static int innermost(const uint8_t *a, uint32_t n, uint32_t pos, uint32_t *body, uint32_t *end, char *name) {
    uint32_t best = 0xFFFFFFFFu;
    int found = 0;
    for (uint32_t i = 0; i + 8 < n && i < pos; i++) {
        uint32_t b, e;
        char nm[5];
        if (!device_at(a, n, i, &b, &e, nm)) continue;
        if (pos < b || pos >= e || e - i >= best) continue;
        best = e - i;
        *body = b;
        *end = e;
        for (int k = 0; k < 5; k++) name[k] = nm[k];
        found = 1;
    }
    return found;
}

static int has_bytes(const uint8_t *a, uint32_t s, uint32_t e, const char *pat, uint32_t pn) {
    for (uint32_t i = s; i + pn <= e; i++) {
        uint32_t k = 0;
        while (k < pn && a[i + k] == (uint8_t)pat[k]) k++;
        if (k == pn) return 1;
    }
    return 0;
}

static uint32_t find_bytes(const uint8_t *a, uint32_t s, uint32_t e, const uint8_t *pat, uint32_t pn) {
    for (uint32_t i = s; i + pn <= e; i++) {
        uint32_t k = 0;
        while (k < pn && a[i + k] == pat[k]) k++;
        if (k == pn) return i;
    }
    return 0xFFFFFFFFu;
}

static int read_int(const uint8_t *p, uint32_t *v) {
    switch (p[0]) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0x0A: *v = p[1]; return 1;
    case 0x0B: *v = (uint32_t)(p[1] | (p[2] << 8)); return 1;
    case 0x0C: *v = (uint32_t)(p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24)); return 1;
    default: return 0;
    }
}

static int named_int(const uint8_t *a, uint32_t n, const uint8_t *seg, uint32_t *v) {
    for (uint32_t i = 0; i + 10 < n; i++) {
        if (a[i] != 0x08) continue;
        if (a[i + 1] != seg[0] || a[i + 2] != seg[1] || a[i + 3] != seg[2] || a[i + 4] != seg[3]) continue;
        if (read_int(a + i + 5, v)) return 1;
    }
    return 0;
}

/* HID descriptor register from _DSM 3CDFF6F7-4267-4555-AD05-B30A3D8938DE
 * function 1: the Return after the first (x == One) test past the UUID. */
static int dsm_hidreg(const uint8_t *a, uint32_t tn, uint32_t s, uint32_t e) {
    static const uint8_t uuid[16] = {
        0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
        0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE
    };
    static const uint8_t dsm_name[4] = {'_', 'D', 'S', 'M'};
    uint32_t m = find_bytes(a, s, e, dsm_name, 4);
    if (m == 0xFFFFFFFFu) return -1;
    uint32_t u = find_bytes(a, m, e, uuid, 16);
    uint32_t from = u != 0xFFFFFFFFu ? u + 16 : m + 4;
    for (uint32_t i = from; i + 6 < e; i++) {
        if (a[i] != 0x93) continue;
        int arg2 = a[i + 1] == 0x6A && a[i + 2] == 0x01;
        int tvar = seg_ok(a + i + 1) && a[i + 5] == 0x01;
        if (!arg2 && !tvar) continue;
        for (uint32_t k = i + 3; k < i + 48 && k + 5 < e; k++) {
            if (a[k] != 0xA4) continue;
            uint32_t v;
            if (read_int(a + k + 1, &v)) return (int)(v & 0xFFFFu);
            if (seg_ok(a + k + 1) && named_int(a, tn, a + k + 1, &v)) return (int)(v & 0xFFFFu);
            return -1;
        }
    }
    return -1;
}

static void scan_resources(const uint8_t *a, uint32_t body, uint32_t end, I2cCand *c) {
    c->gpio = -1;
    for (uint32_t i = body; i + 20 < end; i++) {
        if (a[i] == 0x8E && c->n_res < I2C_MAX_RES) {
            uint32_t len = (uint32_t)(a[i + 1] | (a[i + 2] << 8));
            if (len < 15 || len > 120 || i + 3 + len > end) continue;
            if (a[i + 5] != 1 || a[i + 3] < 1 || a[i + 3] > 2) continue;
            uint32_t tlen = (uint32_t)(a[i + 10] | (a[i + 11] << 8));
            if (tlen < 6 || 12 + tlen > 3 + len) continue;
            int k = c->n_res;
            c->speed[k] = (uint32_t)(a[i + 12] | (a[i + 13] << 8) | (a[i + 14] << 16) | ((uint32_t)a[i + 15] << 24));
            c->addr[k] = (uint16_t)(a[i + 16] | (a[i + 17] << 8));
            const uint8_t *src = a + i + 12 + tlen;
            const uint8_t *last = src;
            for (const uint8_t *q = src; q < a + i + 3 + len && *q; q++)
                if (*q == '.' || *q == '\\' || *q == '^') last = q + 1;
            for (int j = 0; j < 4; j++) c->ctl[k][j] = (char)(last[j] >= 32 && last[j] < 127 ? last[j] : '?');
            c->ctl[k][4] = 0;
            int dup = 0;
            for (int j = 0; j < k; j++) if (c->addr[j] == c->addr[k]) dup = 1;
            if (!dup) c->n_res++;
        } else if (a[i] == 0x8C && c->gpio < 0) {
            uint32_t len = (uint32_t)(a[i + 1] | (a[i + 2] << 8));
            if (len < 20 || len > 200 || i + 3 + len > end || a[i + 4] != 0) continue;
            uint32_t pin_off = (uint32_t)(a[i + 14] | (a[i + 15] << 8));
            if (pin_off < 23 || pin_off + 2 > 3 + len) continue;
            c->gpio = a[i + pin_off] | (a[i + pin_off + 1] << 8);
            c->gpio_flags = a[i + 7];
        }
    }
}

static int add_cand(I2cCand *out, int cap, int *n, const uint8_t *a, uint32_t tn, uint32_t pos) {
    uint32_t body, end;
    char name[5];
    if (!innermost(a, tn, pos, &body, &end, name)) return 0;
    for (int i = 0; i < *n; i++)
        if (!memcmp(out[i].name, name, 5) && out[i].addr[0]) return 0;
    if (*n >= cap) return 0;
    I2cCand *c = &out[*n];
    memset(c, 0, sizeof(*c));
    for (int k = 0; k < 5; k++) c->name[k] = name[k];
    c->score = 1;
    if (has_bytes(a, body, end, "SYNA", 4)) c->score = 4;
    if (has_bytes(a, body, end, "SYNA3530", 8)) c->score = 5;
    else if (has_bytes(a, body, end, "ELAN", 4) || has_bytes(a, body, end, "MSFT", 4)) c->score = 3;
    scan_resources(a, body, end, c);
    c->dsm = dsm_hidreg(a, tn, body, end);
    if (c->n_res) (*n)++;
    return c->n_res;
}

static int scan_table(I2cCand *out, int cap, int *n, const uint8_t *t) __attribute__((noinline));
static int scan_table(I2cCand *out, int cap, int *n, const uint8_t *t) {
    if (!t) return 0;
    uint32_t len = *(const uint32_t *)(t + 4);
    if (len < 40 || len > 0x200000u) return 0;
    const uint8_t *a = t + 36;
    uint32_t tn = len - 36;
    static const uint8_t eisa[5] = {0x0C, 0x41, 0xD0, 0x0C, 0x50};
    for (uint32_t i = 0; i + 8 < tn; i++) {
        if ((a[i] == 'P' && has_bytes(a, i, i + 7, "PNP0C50", 7)) ||
            (a[i] == 'S' && has_bytes(a, i, i + 8, "SYNA3530", 8)) ||
            (a[i] == 0x0C && find_bytes(a, i, i + 5, eisa, 5) == i))
            add_cand(out, cap, n, a, tn, i);
    }
    return 1;
}

int i2cscan_find(I2cCand *out, int cap, int *tables) {
    int n = 0, seen = 0;
    if (scan_table(out, cap, &n, acpi_table("DSDT", 0))) seen++;
    for (int i = 0; i < 31; i++) {
        const uint8_t *t = acpi_table("SSDT", i);
        if (!t) break;
        if (scan_table(out, cap, &n, t)) seen++;
    }
    *tables = seen;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].score > out[i].score) {
                I2cCand t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
    return n;
}

uint32_t i2cscan_mmio(const char *want, int *is_amd) {
    for (int t = 0; t < 32; t++) {
        const uint8_t *tab = t == 0 ? acpi_table("DSDT", 0) : acpi_table("SSDT", t - 1);
        if (!tab) {
            if (t) break;
            continue;
        }
        uint32_t len = *(const uint32_t *)(tab + 4);
        if (len < 40 || len > 0x200000u) continue;
        const uint8_t *a = tab + 36;
        uint32_t n = len - 36;
        for (uint32_t i = 0; i + 8 < n; i++) {
            uint32_t b, e;
            char nm[5];
            if (a[i] != 0x5B || !device_at(a, n, i, &b, &e, nm)) continue;
            if (nm[0] != want[0] || nm[1] != want[1] || nm[2] != want[2] || nm[3] != want[3]) continue;
            for (uint32_t k = b; k + 12 <= e; k++) {
                if (a[k] != 0x86 || a[k + 1] != 0x09 || a[k + 2] != 0x00) continue;
                uint32_t base = (uint32_t)(a[k + 4] | (a[k + 5] << 8) | (a[k + 6] << 16) | ((uint32_t)a[k + 7] << 24));
                if (!base) continue;
                *is_amd = has_bytes(a, b, e, "AMDI0010", 8);
                return base;
            }
        }
    }
    return 0;
}
