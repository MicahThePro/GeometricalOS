#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MALLOC(size) image_memory_alloc(size)
#define STBI_REALLOC(ptr, size) image_memory_realloc((ptr), (size))
#define STBI_FREE(ptr) image_memory_free(ptr)
#define STB_IMAGE_IMPLEMENTATION
#include "image_memory.h"
#include "third_party/stb/stb_image.h"
