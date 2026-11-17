#ifndef GOS_IMAGE_MEMORY_H
#define GOS_IMAGE_MEMORY_H

#include <stddef.h>

void image_memory_reset(void);
void *image_memory_alloc(size_t size);
void *image_memory_realloc(void *ptr, size_t size);
void image_memory_free(void *ptr);

void *gos_image_malloc(size_t size);
void *gos_image_calloc(size_t count, size_t size);
void *gos_image_realloc(void *ptr, size_t size);
void gos_image_free(void *ptr);

#endif
