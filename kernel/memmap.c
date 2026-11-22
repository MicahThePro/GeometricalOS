#include "memmap.h"
#include "bootinfo.h"
#include "serial.h"
#include "xhci.h"

#define MEM_MAP_MAX 128

int mem_type_usable(uint32_t type) {
    return type == 1 || type == 2 || type == 3 || type == 4 || type == 7;
}

static int map_of(const MemEntry **out, int *count) {
    const BootInfo *bi = bootinfo();
    if (!bi || bi->magic != BOOTINFO_MAGIC || !bi->mem_map_count || !bi->mem_map_addr)
        return 0;
    int n = (int)bi->mem_map_count;
    if (n > MEM_MAP_MAX) n = MEM_MAP_MAX;
    *out = (const MemEntry *)(uintptr_t)bi->mem_map_addr;
    *count = n;
    return 1;
}

static int range_hits(uint64_t a, uint64_t a_end, uint64_t b, uint64_t b_end) {
    return a < b_end && b < a_end;
}

static int protected_hit(uint64_t start, uint64_t end) {
    static const uint32_t fixed[][2] = {
        {0x6000u, 0x8000u},
        {0x10000u, 0x81000u},
        {0x81000u, 0x85000u},
        {0x8C000u, 0x91000u},
        {0x1800000u, 0x1808000u},
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (range_hits(start, end, fixed[i][0], fixed[i][1])) return 1;
    }
    const BootInfo *bi = bootinfo();
    if (bi->magic != BOOTINFO_MAGIC) return 0;
    if (bi->image_arena) {
        uint64_t bytes = (uint64_t)bi->image_arena_pages * 4096ull;
        if (range_hits(start, end, bi->image_arena, bi->image_arena + bytes)) return 1;
    }
    if (bi->xhci_heap) {
        uint64_t b = bi->xhci_heap;
        if (range_hits(start, end, b, b + 960u * 1024u)) return 1;
    }
    if (bi->efi_file_addr && bi->efi_file_size) {
        uint64_t b = bi->efi_file_addr;
        if (range_hits(start, end, b, b + bi->efi_file_size)) return 1;
    }
    if (bi->lfb && bi->pitch && bi->height) {
        uint64_t b = bi->lfb;
        uint64_t bytes = (uint64_t)bi->pitch * bi->height;
        if (range_hits(start, end, b, b + bytes)) return 1;
    }
    return 0;
}

int mem_span_ok(uint64_t start, uint64_t bytes) {
    const BootInfo *bi = bootinfo();
    if (!bytes) return 1;
    if (start + bytes < start) return 0;
    const MemEntry *ents = 0;
    int count = 0;
    if (!map_of(&ents, &count)) {
        if (!bi || bi->magic != BOOTINFO_MAGIC) return 0;
        uint64_t memory_end = 0x100000ull + (uint64_t)bi->ext_mem_kb * 1024u;
        return memory_end >= start + bytes;
    }
    uint64_t cursor = start;
    uint64_t end = start + bytes;
    while (cursor < end) {
        int found = 0;
        for (int i = 0; i < count; i++) {
            if (!mem_type_usable(ents[i].type)) continue;
            uint64_t base = ents[i].base;
            uint64_t rend = base + ents[i].length;
            if (cursor >= base && cursor < rend) {
                cursor = rend < end ? rend : end;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return !protected_hit(start, end);
}

/* Walk usable regions below limit. This does not know about kernel reservations;
 * mem_alloc adds those. The hole self-test calls this directly. */
uint32_t mem_alloc_from(const MemEntry *ents, int count, uint32_t bytes,
                        uint32_t align, uint64_t limit) {
    if (!bytes || !align || (align & (align - 1u))) return 0;
    for (int i = 0; i < count; i++) {
        if (!mem_type_usable(ents[i].type)) continue;
        uint64_t region = ents[i].base;
        uint64_t end = region + ents[i].length;
        if (region >= limit) continue;
        if (end > limit) end = limit;
        uint64_t aligned = (region + align - 1u) & ~((uint64_t)align - 1u);
        if (aligned < region || aligned >= end || aligned + bytes > end) continue;
        return (uint32_t)aligned;
    }
    return 0;
}

void *mem_alloc(uint32_t bytes, uint32_t align) {
    const MemEntry *ents = 0;
    int count = 0;
    if (!align) align = 1;
    if (!map_of(&ents, &count)) return 0;
    for (int i = 0; i < count; i++) {
        if (!mem_type_usable(ents[i].type)) continue;
        uint64_t cursor = ents[i].base;
        uint64_t end = cursor + ents[i].length;
        if (end > 0x100000000ull) end = 0x100000000ull;
        while (cursor < end) {
            uint64_t aligned = (cursor + align - 1u) & ~((uint64_t)align - 1u);
            if (aligned < cursor || aligned >= end || aligned + bytes > end) break;
            if (!protected_hit(aligned, aligned + bytes))
                return (void *)(uintptr_t)aligned;
            uint64_t next = aligned + 4096u;
            if (next <= cursor) break;
            cursor = next;
        }
    }
    return 0;
}

void *mem_buffer(uint32_t fixed, uint32_t bytes) {
    if (mem_span_ok(fixed, bytes)) return (void *)(uintptr_t)fixed;
    return mem_alloc(bytes, 4096u);
}

static int expect_eq(const char *name, uint32_t got, uint32_t want) {
    if (got == want) return 1;
    serial_write(name);
    serial_write(" got ");
    serial_hex32(got);
    serial_write(" want ");
    serial_hex32(want);
    serial_write("\n");
    return 0;
}

void mem_selftest(void) {
    MemEntry fake[3];
    fake[0].base = 0x100000;
    fake[0].length = 0x100000;
    fake[0].type = 7;
    fake[1].base = 0x200000;
    fake[1].length = 0x100000;
    fake[1].type = 0;
    fake[2].base = 0x300000;
    fake[2].length = 0x200000;
    fake[2].type = 7;
    uint32_t got = mem_alloc_from(fake, 3, 0x180000u, 0x1000u, 0x100000000ull);
    int ok = expect_eq("ALLOC HOLE", got, 0x300000u);
    ok = expect_eq("SCRATCH 31", xhci_scratchpad_count(31u << 27), 31u) && ok;
    ok = expect_eq("SCRATCH 1023", xhci_scratchpad_count((31u << 27) | (31u << 21)), 1023u) && ok;
    ok = expect_eq("SCRATCH 40", xhci_scratchpad_count((8u << 27) | (1u << 21)), 40u) && ok;
    serial_write(ok ? "ALLOC HOLE TEST ok\n" : "ALLOC HOLE TEST FAIL\n");

    const MemEntry *ents = 0;
    int count = 0;
    if (!map_of(&ents, &count)) {
        serial_write("ALLOC MAP HOLE bios\n");
        return;
    }
    int saw = 0;
    for (int i = 0; i < count; i++) {
        if (mem_type_usable(ents[i].type)) continue;
        if (ents[i].base >= 0x100000000ull || ents[i].length < 4096) continue;
        uint64_t at = ents[i].base & ~0xFFFull;
        if (at < ents[i].base) at += 4096;
        if (at + 4096 > ents[i].base + ents[i].length) continue;
        saw = 1;
        if (mem_span_ok(at, 4096)) {
            serial_write("ALLOC MAP HOLE FAIL ");
            serial_hex32((uint32_t)at);
            serial_write("\n");
            return;
        }
        break;
    }
    serial_write(saw ? "ALLOC MAP HOLE ok\n" : "ALLOC MAP HOLE none\n");
}
