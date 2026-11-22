#include "image_memory.h"
#include "bootinfo.h"
#include "string.h"
#include "third_party/libwebp/src/utils/random_utils.h"
#include <stdint.h>

#define IMAGE_ARENA_ADDRESS 0x800000u
#define IMAGE_ARENA_SIZE (15u * 1024u * 1024u)
#define IMAGE_ALLOC_MAGIC 0x494D474Du

typedef struct {
    uint32_t size;
    uint32_t magic;
    uint32_t free;
    uint32_t reserved;
} ImageAllocHeader;

static uint32_t arena_used;

static uint8_t *image_arena(void) {
    const BootInfo *bi = bootinfo();
    if (bi->magic == BOOTINFO_MAGIC && bi->mem_map_count) {
        if (!bi->image_arena) return 0;
        return (uint8_t *)(uintptr_t)bi->image_arena;
    }
    return (uint8_t *)IMAGE_ARENA_ADDRESS;
}

void image_memory_reset(void) {
    arena_used = 0;
}

void *image_memory_alloc(size_t size) {
    if (size == 0 || size > UINT32_MAX - sizeof(ImageAllocHeader) - 7u)
        return 0;
    uint32_t requested = ((uint32_t)size + 7u) & ~7u;
    uint8_t *arena = image_arena();
    if (!arena) return 0;
    uint32_t offset = 0;
    while (offset < arena_used) {
        if (arena_used - offset < sizeof(ImageAllocHeader)) return 0;
        ImageAllocHeader *header = (ImageAllocHeader *)(arena + offset);
        if (header->magic != IMAGE_ALLOC_MAGIC ||
            header->size > arena_used - offset - sizeof(*header))
            return 0;
        uint32_t total = sizeof(*header) + header->size;
        if (header->free && header->size >= requested) {
            uint32_t remainder = header->size - requested;
            if (remainder >= sizeof(*header) + 8u) {
                ImageAllocHeader *next = (ImageAllocHeader *)
                    ((uint8_t *)(header + 1) + requested);
                next->size = remainder - sizeof(*next);
                next->magic = IMAGE_ALLOC_MAGIC;
                next->free = 1;
                next->reserved = 0;
                header->size = requested;
            }
            header->free = 0;
            return header + 1;
        }
        offset += total;
    }

    uint32_t total = sizeof(ImageAllocHeader) + requested;
    if (total > IMAGE_ARENA_SIZE - arena_used) return 0;
    ImageAllocHeader *header = (ImageAllocHeader *)(arena + arena_used);
    header->size = requested;
    header->magic = IMAGE_ALLOC_MAGIC;
    header->free = 0;
    header->reserved = 0;
    arena_used += total;
    return header + 1;
}

void image_memory_free(void *ptr) {
    if (!ptr) return;
    uint8_t *arena = image_arena();
    if (!arena) return;
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)arena + sizeof(ImageAllocHeader);
    uintptr_t end = (uintptr_t)arena + arena_used;
    if (address < start || address > end) return;
    ImageAllocHeader *header = (ImageAllocHeader *)ptr - 1;
    if (header->magic != IMAGE_ALLOC_MAGIC || header->free) return;
    header->free = 1;

    uint32_t offset = 0;
    while (offset < arena_used) {
        if (arena_used - offset < sizeof(ImageAllocHeader)) return;
        ImageAllocHeader *current = (ImageAllocHeader *)(arena + offset);
        if (current->magic != IMAGE_ALLOC_MAGIC ||
            current->size > arena_used - offset - sizeof(*current))
            return;
        uint32_t total = sizeof(*current) + current->size;
        uint32_t next_offset = offset + total;
        if (current->free && next_offset < arena_used) {
            if (arena_used - next_offset < sizeof(ImageAllocHeader)) return;
            ImageAllocHeader *next = (ImageAllocHeader *)(arena + next_offset);
            if (next->magic != IMAGE_ALLOC_MAGIC) return;
            if (next->free) {
                current->size += sizeof(*next) + next->size;
                continue;
            }
        }
        offset = next_offset;
    }
}

void *image_memory_realloc(void *ptr, size_t size) {
    if (!ptr) return image_memory_alloc(size);
    if (size == 0) return 0;
    ImageAllocHeader *header = (ImageAllocHeader *)ptr - 1;
    if (header->magic != IMAGE_ALLOC_MAGIC || header->free) return 0;
    if (size <= header->size) return ptr;
    uint8_t *arena = image_arena();
    if (!arena) return 0;
    uint8_t *next_address = (uint8_t *)(header + 1) + header->size;
    if (next_address < arena + arena_used) {
        ImageAllocHeader *next = (ImageAllocHeader *)next_address;
        if (next->magic == IMAGE_ALLOC_MAGIC && next->free &&
            (uint64_t)header->size + sizeof(*next) + next->size >= size) {
            header->size += sizeof(*next) + next->size;
            header->free = 0;
            return ptr;
        }
    }
    void *next = image_memory_alloc(size);
    if (!next) return 0;
    size_t copy = header->size < size ? header->size : size;
    memcpy(next, ptr, copy);
    image_memory_free(ptr);
    return next;
}

void *gos_image_malloc(size_t size) {
    return image_memory_alloc(size);
}

void *gos_image_calloc(size_t count, size_t size) {
    if (size && count > UINT32_MAX / size) return 0;
    size_t total = count * size;
    void *ptr = image_memory_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *gos_image_realloc(void *ptr, size_t size) {
    return image_memory_realloc(ptr, size);
}

void gos_image_free(void *ptr) {
    image_memory_free(ptr);
}

int abs(int value) {
    return value < 0 ? -value : value;
}

void VP8InitRandom(VP8Random *random, float dithering) {
    /* The viewer uses WebP's default zero-dithering decode options. */
    (void)dithering;
    memset(random, 0, sizeof(*random));
    random->index2_ = 31;
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    uint8_t *bytes = (uint8_t *)base;
    if (!bytes || !compare || size == 0) return;
    for (size_t i = 1; i < count; i++) {
        size_t j = i;
        while (j > 0 &&
               compare(bytes + (j - 1) * size, bytes + j * size) > 0) {
            uint8_t *left = bytes + (j - 1) * size;
            uint8_t *right = bytes + j * size;
            for (size_t k = 0; k < size; k++) {
                uint8_t temp = left[k];
                left[k] = right[k];
                right[k] = temp;
            }
            j--;
        }
    }
}
