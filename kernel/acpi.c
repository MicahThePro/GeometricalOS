#include "acpi.h"
#include "bootinfo.h"
#include "io.h"
#include "input.h"
#include "serial.h"

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

static char steps[8][72];
static int step_n;

static int bytes_sum_ok(const uint8_t *p, uint32_t n) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static void alog(const char *s) {
    serial_write("ACPI: ");
    serial_write(s);
    serial_write("\n");
    if (step_n >= (int)(sizeof(steps) / sizeof(steps[0]))) return;
    int i = 0;
    for (; s[i] && i < 70; i++) steps[step_n][i] = s[i];
    steps[step_n][i] = 0;
    step_n++;
}

int acpi_step_count(void) { return step_n; }
const char *acpi_step_line(int index) {
    if (index < 0 || index >= step_n) return "";
    return steps[index];
}

static uint32_t find_rsdp(void) {
    const BootInfo *bi = bootinfo();
    if (bi->magic == BOOTINFO_MAGIC && bi->rsdp_phys) return bi->rsdp_phys;
    for (uint32_t a = 0xE0000u; a < 0x100000u; a += 16u) {
        const uint8_t *s = (const uint8_t *)(uintptr_t)a;
        if (s[0] == 'R' && s[1] == 'S' && s[2] == 'D' && s[3] == ' ' &&
            s[4] == 'P' && s[5] == 'T' && s[6] == 'R' && s[7] == ' ') {
            if (bytes_sum_ok(s, 20)) return a;
        }
    }
    return 0;
}

static uint32_t find_facp(uint32_t root, int wide) {
    if (root < 0x1000u) return 0;
    AcpiHeader *hdr = (AcpiHeader *)(uintptr_t)root;
    if (hdr->length < 36 || hdr->length > 0x10000u) return 0;
    if (!bytes_sum_ok((const uint8_t *)hdr, hdr->length)) return 0;
    uint32_t entry = wide ? 8u : 4u;
    uint32_t n = (hdr->length - 36u) / entry;
    if (n > 64) n = 64;
    const uint8_t *p = (const uint8_t *)hdr + 36;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t phys = wide ? *(const uint64_t *)(p + i * 8u)
                             : *(const uint32_t *)(p + i * 4u);
        if (phys < 0x1000ull || phys > 0xFFFFF000ull) continue;
        AcpiHeader *child = (AcpiHeader *)(uintptr_t)phys;
        if (child->sig[0] == 'F' && child->sig[1] == 'A' &&
            child->sig[2] == 'C' && child->sig[3] == 'P') {
            if (child->length >= 116 && bytes_sum_ok((const uint8_t *)child, child->length))
                return (uint32_t)phys;
        }
    }
    return 0;
}

static void hex4(char *d, int *n, uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    for (int s = 12; s >= 0; s -= 4) d[(*n)++] = h[(v >> s) & 0xF];
}

int acpi_irq1(uint32_t *ioapic, uint32_t *gsi, int *level, int *low) {
    if (ioapic) *ioapic = 0;
    if (gsi) *gsi = 1;
    if (level) *level = 0;
    if (low) *low = 0;
    uint32_t rsdp_phys = find_rsdp();
    if (!rsdp_phys) return 0;
    Rsdp *rsdp = (Rsdp *)(uintptr_t)rsdp_phys;
    uint32_t root = 0;
    int wide = 0;
    if (rsdp->revision >= 2 && rsdp->length >= 36 &&
        bytes_sum_ok((const uint8_t *)rsdp, rsdp->length) &&
        rsdp->xsdt && rsdp->xsdt <= 0xFFFFF000ull) {
        root = (uint32_t)rsdp->xsdt;
        wide = 1;
    } else if (bytes_sum_ok((const uint8_t *)rsdp, 20)) {
        root = rsdp->rsdt;
    }
    if (!root) return 0;
    AcpiHeader *xs = (AcpiHeader *)(uintptr_t)root;
    if (xs->length < 36 || xs->length > 0x10000u) return 0;
    uint32_t entry = wide ? 8u : 4u;
    uint32_t n = (xs->length - 36u) / entry;
    if (n > 64) n = 64;
    const uint8_t *p = (const uint8_t *)xs + 36;
    uint32_t madt = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t phys = wide ? *(const uint64_t *)(p + i * 8u)
                             : *(const uint32_t *)(p + i * 4u);
        if (phys < 0x1000ull || phys > 0xFFFFF000ull) continue;
        AcpiHeader *child = (AcpiHeader *)(uintptr_t)phys;
        if (child->sig[0] == 'A' && child->sig[1] == 'P' &&
            child->sig[2] == 'I' && child->sig[3] == 'C' &&
            child->length >= 44 && child->length <= 0x1000u &&
            bytes_sum_ok((const uint8_t *)child, child->length)) {
            madt = (uint32_t)phys;
            break;
        }
    }
    if (!madt) return 0;
    const uint8_t *m = (const uint8_t *)(uintptr_t)madt;
    uint32_t len = *(const uint32_t *)(m + 4);
    uint32_t off = 44;
    uint32_t apic = 0;
    uint32_t pin = 1;
    int trig = 0, pol = 0, have = 0;
    while (off + 2 < len) {
        uint8_t type = m[off];
        uint8_t elen = m[off + 1];
        if (elen < 2 || off + elen > len) break;
        if (type == 1 && elen >= 12 && !apic) {
            apic = *(const uint32_t *)(m + off + 4);
            have = 1;
        } else if (type == 2 && elen >= 10 && m[off + 3] == 1) {
            pin = *(const uint32_t *)(m + off + 4);
            uint16_t flags = *(const uint16_t *)(m + off + 8);
            int polarity = flags & 3;
            int trigger = (flags >> 2) & 3;
            pol = polarity == 3;
            trig = trigger == 3;
            have = 1;
        }
        off += elen;
    }
    if (ioapic) *ioapic = apic;
    if (gsi) *gsi = pin;
    if (level) *level = trig;
    if (low) *low = pol;
    return have;
}

void acpi_bringup(void) {
    step_n = 0;
    uint32_t rsdp_phys = find_rsdp();
    if (!rsdp_phys) {
        alog("no RSDP");
        return;
    }
    Rsdp *rsdp = (Rsdp *)(uintptr_t)rsdp_phys;
    uint32_t facp = 0;
    if (rsdp->revision >= 2 && rsdp->length >= 36 &&
        bytes_sum_ok((const uint8_t *)rsdp, rsdp->length) &&
        rsdp->xsdt && rsdp->xsdt <= 0xFFFFF000ull)
        facp = find_facp((uint32_t)rsdp->xsdt, 1);
    if (!facp && bytes_sum_ok((const uint8_t *)rsdp, 20))
        facp = find_facp(rsdp->rsdt, 0);
    if (!facp) {
        alog("no FADT");
        return;
    }
    const uint8_t *f = (const uint8_t *)(uintptr_t)facp;
    uint32_t len = *(const uint32_t *)(f + 4);
    uint32_t smi = *(const uint32_t *)(f + 48);
    uint8_t enable = f[52];
    uint32_t pm1 = *(const uint32_t *)(f + 64);
    char line[72];
    int n = 0;
    const char *p = "IAPC ";
    while (*p) line[n++] = *p++;
    if (len >= 112) {
        uint16_t boot = *(const uint16_t *)(f + 109);
        hex4(line, &n, boot);
        p = (boot & 2) ? " 8042=Y" : " 8042=N";
    } else {
        p = "short 8042=?";
    }
    while (*p) line[n++] = *p++;
    line[n] = 0;
    alog(line);

    if (!pm1 || pm1 > 0xFFF0u) {
        alog("PM1 none");
        return;
    }
    uint16_t before = inw((uint16_t)pm1);
    uint16_t after = before;
    const char *act = " skip";
    if (before & 1) {
        act = " already";
    } else if (smi && enable && smi <= 0xFFF0u) {
        outb((uint16_t)smi, enable);
        act = " wrote";
        for (int i = 0; i < 20; i++) {
            timer_wait_ms(5);
            after = inw((uint16_t)pm1);
            if (after & 1) break;
        }
    }
    n = 0;
    p = "SCI ";
    while (*p) line[n++] = *p++;
    hex4(line, &n, before);
    line[n++] = '>';
    hex4(line, &n, after);
    while (*act) line[n++] = *act++;
    line[n] = 0;
    alog(line);
}
