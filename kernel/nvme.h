#ifndef GOS_NVME_H
#define GOS_NVME_H

#include <stdint.h>

int nvme_init(void);
int nvme_read(uint64_t lba, void *buf, uint32_t count);
int nvme_write(uint64_t lba, const void *buf, uint32_t count);

/* A GeoFS filesystem already written on this SSD. */
int nvme_find_geofs(uint64_t *start, uint32_t *sectors);
/* One empty partition, 32 MB to 16 GB, that does not contain Windows. */
int nvme_find_empty(uint64_t *start, uint32_t *sectors);
/* Copy the USB boot file onto the Windows EFI partition as \GEOMOS\BOOTX64.EFI. */
int nvme_store_bootloader(void);

#endif
