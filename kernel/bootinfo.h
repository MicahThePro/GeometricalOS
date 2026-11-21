#ifndef GOS_BOOTINFO_H
#define GOS_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_ADDR 0x6000u
#define BOOTINFO_MAGIC 0xB007DA7Au

typedef struct {
    uint32_t magic;
    uint32_t from_hdd;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t lfb;
    uint32_t red_size;
    uint32_t red_pos;
    uint32_t green_size;
    uint32_t green_pos;
    uint32_t blue_size;
    uint32_t blue_pos;
    uint32_t ext_mem_kb;
    uint32_t font_addr;
    uint32_t font_height;
    uint32_t boot_drive;
    /* Filled by the UEFI loader. Zero on a BIOS boot. xhci_pci has bit 31
     * set when bus/dev/fn are valid. The MMIO address is already mapped. */
    uint32_t xhci_mmio;
    uint32_t xhci_pci;
} BootInfo;

static inline const BootInfo *bootinfo(void) {
    return (const BootInfo *)BOOTINFO_ADDR;
}

#endif
