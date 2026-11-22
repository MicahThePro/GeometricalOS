#ifndef GOS_MEMMAP_H
#define GOS_MEMMAP_H

#include <stdint.h>

/* One UEFI memory descriptor, copied before ExitBootServices.
 * type is the firmware type: 1, 2, 3, 4 and 7 are usable after exit.
 * On a BIOS boot the count is zero and the old ext_mem_kb check remains. */
typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} MemEntry;

int mem_type_usable(uint32_t type);
int mem_span_ok(uint64_t start, uint64_t bytes);
void *mem_alloc(uint32_t bytes, uint32_t align);
void *mem_buffer(uint32_t fixed, uint32_t bytes);
uint32_t mem_alloc_from(const MemEntry *ents, int count, uint32_t bytes,
                        uint32_t align, uint64_t limit);
void mem_selftest(void);

#endif
