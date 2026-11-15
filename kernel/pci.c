#include "pci.h"
#include "io.h"

uint32_t pci_read(int bus, int dev, int fn, int off) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void pci_write(int bus, int dev, int fn, int off, uint32_t val) {
    uint32_t addr = 0x80000000u
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | ((uint32_t)off & 0xFCu);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

int pci_find(uint16_t vendor, uint16_t device, int *bus, int *dev, int *fn) {
    for (int b = 0; b < 8; b++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                uint32_t id = pci_read(b, d, f, 0);
                if (id == 0xFFFFFFFFu || id == 0) {
                    if (f == 0) break;
                    continue;
                }
                if ((uint16_t)id == vendor && (uint16_t)(id >> 16) == device) {
                    if (bus) *bus = b;
                    if (dev) *dev = d;
                    if (fn) *fn = f;
                    return 0;
                }
                if (f == 0 && !(pci_read(b, d, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    return -1;
}
