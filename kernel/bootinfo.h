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
    /* Raw BOOTX64.EFI copied by the loader, and the NVMe controller. */
    uint32_t efi_file_addr;
    uint32_t efi_file_size;
    uint32_t nvme_mmio;
    uint32_t nvme_pci;
    /* Filled by the UEFI loader. Zero on a BIOS boot. */
    uint32_t fw_revision;
    uint32_t gop_mode;
    uint32_t usable_regions;
    uint32_t largest_base;
    uint32_t largest_pages;
    uint32_t overlap_mask;
    char fw_vendor[64];
    /* Usable pages whose region starts at or above 4 GB. Zero on BIOS. */
    uint32_t high_pages;
    /* Compact MemEntry array. Both zero on a BIOS boot. */
    uint32_t mem_map_addr;
    uint32_t mem_map_count;
    /* Image arena from AllocatePages. Zero keeps the BIOS address. */
    uint32_t image_arena;
    uint32_t image_arena_pages;
    /* ACPI RSDP. Zero if the firmware did not publish one below 4 GB. */
    uint32_t rsdp_phys;
    /* USB DMA heap. 0x1100000 when that range could be reserved. */
    uint32_t xhci_heap;
} BootInfo;

typedef char bootinfo_high_ok[(__builtin_offsetof(BootInfo, high_pages) == 180) ? 1 : -1];
typedef char bootinfo_map_ok[(__builtin_offsetof(BootInfo, mem_map_addr) == 184) ? 1 : -1];
typedef char bootinfo_fits[(sizeof(BootInfo) <= 256) ? 1 : -1];

static inline const BootInfo *bootinfo(void) {
    return (const BootInfo *)BOOTINFO_ADDR;
}

#endif
