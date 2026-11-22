#include "hpet.h"
#include "bootinfo.h"

typedef struct __attribute__((packed)) {
    char sig[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
} Rsdp;

typedef struct __attribute__((packed)) {
    char sig[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t rest[26];
} AcpiHeader;

static uint32_t hpet_base;
static uint32_t hpet_period_fs;
static int hpet_ready;

static int bytes_sum_ok(const uint8_t *p, uint32_t n) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

uint64_t hpet_ticks_for(uint64_t period_fs, uint32_t ms) {
    if (!period_fs || period_fs > 0x05F5E100ull) return 0;
    return (1000000000000ull / period_fs) * ms;
}

static uint32_t find_hpet(uint32_t table, int wide) {
    if (table < 0x1000u) return 0;
    AcpiHeader *hdr = (AcpiHeader *)(uintptr_t)table;
    if (hdr->length < 36 || hdr->length > 0x10000u) return 0;
    if (!bytes_sum_ok((const uint8_t *)hdr, hdr->length)) return 0;
    uint32_t entry = wide ? 8u : 4u;
    if (hdr->length < 36 + entry) return 0;
    uint32_t n = (hdr->length - 36u) / entry;
    if (n > 64) n = 64;
    const uint8_t *p = (const uint8_t *)hdr + 36;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t phys = wide ? *(const uint64_t *)(p + i * 8u) : *(const uint32_t *)(p + i * 4u);
        if (phys < 0x1000ull || phys > 0xFFFFF000ull) continue;
        AcpiHeader *child = (AcpiHeader *)(uintptr_t)phys;
        if (child->length < 52 || child->length > 0x1000u) continue;
        if (child->sig[0] != 'H' || child->sig[1] != 'P' || child->sig[2] != 'E' || child->sig[3] != 'T')
            continue;
        if (!bytes_sum_ok((const uint8_t *)child, child->length)) continue;
        uint64_t addr = *(const uint64_t *)((const uint8_t *)child + 44);
        if (addr < 0x100000ull || addr > 0xFFFFF000ull) continue;
        return (uint32_t)addr;
    }
    return 0;
}

static uint32_t locate_base(void) {
    const BootInfo *bi = bootinfo();
    uint32_t rsdp_phys = 0;
    if (bi->magic == BOOTINFO_MAGIC) rsdp_phys = bi->rsdp_phys;
    if (!rsdp_phys) {
        for (uint32_t a = 0xE0000u; a < 0x100000u; a += 16u) {
            const char *s = (const char *)(uintptr_t)a;
            if (s[0] == 'R' && s[1] == 'S' && s[2] == 'D' && s[3] == ' ' &&
                s[4] == 'P' && s[5] == 'T' && s[6] == 'R' && s[7] == ' ') {
                if (bytes_sum_ok((const uint8_t *)s, 20)) {
                    rsdp_phys = a;
                    break;
                }
            }
        }
    }
    if (!rsdp_phys) return 0;
    Rsdp *rsdp = (Rsdp *)(uintptr_t)rsdp_phys;
    if (rsdp->sig[0] != 'R' || rsdp->sig[1] != 'S') return 0;
    if (rsdp->revision >= 2 && rsdp->length >= 36 &&
        bytes_sum_ok((const uint8_t *)rsdp, rsdp->length) &&
        rsdp->xsdt && rsdp->xsdt <= 0xFFFFF000ull) {
        uint32_t base = find_hpet((uint32_t)rsdp->xsdt, 1);
        if (base) return base;
    }
    if (!bytes_sum_ok((const uint8_t *)rsdp, 20)) return 0;
    return find_hpet(rsdp->rsdt, 0);
}

int hpet_prepare(void) {
    if (hpet_ready) return hpet_base != 0;
    hpet_ready = 1;
    hpet_base = locate_base();
    if (!hpet_base) return 0;
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)hpet_base;
    uint32_t period = reg[1];
    if (period < 100000u || period > 0x05F5E100u) {
        hpet_base = 0;
        return 0;
    }
    hpet_period_fs = period;
    reg[4] |= 1u;
    return 1;
}

int hpet_present(void) {
    return hpet_prepare() && hpet_base != 0;
}

uint32_t hpet_period(void) {
    return hpet_present() ? hpet_period_fs : 0;
}

uint64_t hpet_counter(void) {
    if (!hpet_base) return 0;
    volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)(hpet_base + 0xF0u);
    uint32_t lo = reg[0];
    uint32_t hi = reg[1];
    return ((uint64_t)hi << 32) | lo;
}
