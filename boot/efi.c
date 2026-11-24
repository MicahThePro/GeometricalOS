/* 64-bit UEFI loader. Rufus copies this file to \EFI\BOOT\BOOTX64.EFI
 * on a FAT32 stick of any size, including 64 GB. The kernel image is inside
 * this file, so the stick does not need the CD boot catalog.
 */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint64_t uintn;
typedef uint64_t efi_status;
typedef void *efi_handle;

#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EfiTableHeader;

struct EfiSimpleTextOutput;
typedef struct EfiSimpleTextOutput {
    void *reset;
    efi_status (*output_string)(struct EfiSimpleTextOutput *self, uint16_t *text);
} EfiSimpleTextOutput;

typedef struct {
    EfiTableHeader hdr;
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t pad0;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    EfiSimpleTextOutput *con_out;
    efi_handle stderr_handle;
    void *std_err;
    void *runtime;
    void *boot;
    uintn entries;
    void *config;
} EfiSystemTable;

typedef struct {
    EfiTableHeader hdr;
    void *raise_tpl;
    void *restore_tpl;
    efi_status (*allocate_pages)(uint32_t type, uint32_t memory_type, uintn pages, uint64_t *memory);
    void *free_pages;
    efi_status (*get_memory_map)(uintn *size, void *map, uintn *key, uintn *desc_size, uint32_t *desc_ver);
    void *allocate_pool;
    void *free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol;
    void *reinstall_protocol;
    void *uninstall_protocol;
    void *handle_protocol;
    void *reserved;
    void *register_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_config;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status (*exit_boot_services)(efi_handle image, uintn key);
    void *next_monotonic;
    efi_status (*stall)(uintn microseconds);
    efi_status (*set_watchdog)(uintn timeout, uint64_t code, uintn data_size, uint16_t *data);
    void *connect;
    void *disconnect;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_info;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status (*locate_protocol)(uint8_t *guid, void *reg, void **iface);
} EfiBootServices;

typedef struct {
    uint32_t red, green, blue, reserved;
} EfiPixelMask;

typedef struct {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    EfiPixelMask mask;
    uint32_t stride;
} EfiGopInfo;

typedef struct {
    uint32_t max_mode;
    uint32_t mode;
    EfiGopInfo *info;
    uintn size_of_info;
    uint64_t framebuffer;
    uintn framebuffer_size;
} EfiGopMode;

typedef struct EfiGop {
    efi_status (*query_mode)(struct EfiGop *self, uint32_t mode, uintn *size, EfiGopInfo **info);
    efi_status (*set_mode)(struct EfiGop *self, uint32_t mode);
    void *blt;
    EfiGopMode *mode;
} EfiGop;

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical;
    uint64_t virtual;
    uint64_t pages;
    uint64_t attribute;
} EfiMemDesc;

typedef char conout_ok[(__builtin_offsetof(EfiSystemTable, con_out) == 0x40) ? 1 : -1];
typedef char boot_ok[(__builtin_offsetof(EfiSystemTable, boot) == 0x60) ? 1 : -1];
typedef char locate_ok[(__builtin_offsetof(EfiBootServices, locate_protocol) == 0x140) ? 1 : -1];
typedef char exit_ok[(__builtin_offsetof(EfiBootServices, exit_boot_services) == 0xE8) ? 1 : -1];
typedef char alloc_ok[(__builtin_offsetof(EfiBootServices, allocate_pages) == 0x28) ? 1 : -1];

static uint8_t gop_guid[16] = {
    0xDE, 0xA9, 0x42, 0x90, 0xDC, 0x23, 0x38, 0x4A,
    0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A
};

extern char _binary_bootimg_bin_start[];
extern char _binary_bootimg_bin_end[];
extern char _binary_switch64_bin_start[];
extern char _binary_switch64_bin_end[];
extern char _binary_enter32_bin_start[];
extern char _binary_enter32_bin_end[];
extern void efi_jump_low(uint64_t cr3);

static void *memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *memset(void *dst, int c, uint64_t n) {
    uint8_t *d = dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8, 1);
    outb(0x3F8 + 1, 0);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
}
static void sput(char c) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x3F8 + 5) & 0x20) break;
    }
    outb(0x3F8, (uint8_t)c);
}
static void sputs(const char *s) {
    while (*s) sput(*s++);
}

static uint32_t pci_read(int bus, int dev, int fn, int off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | ((uint32_t)off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static int find_pci(uint8_t want_base, uint8_t want_sub, uint8_t want_prog,
                    int *bus, int *dev, int *fn, uint64_t *bar, uint32_t *size, uint32_t bar_size) {
    for (int b = 0; b < 256; b++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                uint32_t id = pci_read(b, d, f, 0);
                if (id == 0xFFFFFFFFu || id == 0) {
                    if (f == 0) break;
                    continue;
                }
                uint32_t class = pci_read(b, d, f, 8);
                uint8_t base = (uint8_t)(class >> 24);
                uint8_t sub = (uint8_t)(class >> 16);
                uint8_t prog = (uint8_t)(class >> 8);
                if (base == want_base && sub == want_sub && prog == want_prog) {
                    uint32_t lo = pci_read(b, d, f, 0x10);
                    uint32_t hi = 0;
                    if ((lo & 0x7u) == 0x4u) hi = pci_read(b, d, f, 0x14);
                    if ((lo & 1u) == 0) {
                        *bus = b;
                        *dev = d;
                        *fn = f;
                        *bar = ((uint64_t)hi << 32) | (lo & ~0xFu);
                        *size = bar_size;
                        return 1;
                    }
                }
                if (f == 0 && !(pci_read(b, d, 0, 0x0C) & 0x00800000u)) break;
            }
        }
    }
    return 0;
}

static uint64_t *pd[4];

static void build_pages(void) {
    uint64_t *pml4 = (uint64_t *)0x1800000;
    uint64_t *pdpt = (uint64_t *)0x1801000;
    memset(pml4, 0, 4096);
    memset(pdpt, 0, 4096);
    pml4[0] = 0x1801000ULL | 3ULL;
    for (int i = 0; i < 4; i++) {
        pd[i] = (uint64_t *)(0x1802000ULL + (uint64_t)i * 4096ULL);
        memset(pd[i], 0, 4096);
        pdpt[i] = (0x1802000ULL + (uint64_t)i * 4096ULL) | 3ULL;
        for (int j = 0; j < 512; j++) {
            uint64_t phys = (((uint64_t)i * 512ULL + (uint64_t)j) << 21);
            pd[i][j] = phys | 0x83ULL;
        }
    }
    /* USB rings and DMA buffers. Uncached so the controller sees writes. */
    pd[0][8] = (8ULL << 21) | 0x83ULL | 0x18ULL;
}

static uint32_t map_device(uint64_t phys, uint32_t bytes, uint32_t window) {
    if (!phys || !bytes) return 0;
    if (bytes > 16u * 1024u * 1024u) bytes = 16u * 1024u * 1024u;
    if (phys < 0x100000000ULL) {
        uint64_t end = phys + bytes;
        for (uint64_t a = phys & ~0x1FFFFFULL; a < end; a += 0x200000ULL) {
            if (a < 0x2000000ULL) continue;
            uint32_t gi = (uint32_t)((a >> 30) & 3ULL);
            uint32_t idx = (uint32_t)((a >> 21) & 0x1FFULL);
            pd[gi][idx] = a | 0x83ULL | 0x18ULL;
        }
        return (uint32_t)phys;
    }
    uint64_t base = phys & ~0x1FFFFFULL;
    uint32_t off = (uint32_t)(phys - base);
    uint32_t pages = (uint32_t)((bytes + off + 0x1FFFFFu) >> 21);
    uint32_t idx = (window >> 21) & 0x1FF;
    for (uint32_t i = 0; i < pages && idx + i < 512; i++)
        pd[3][idx + i] = (base + ((uint64_t)i << 21)) | 0x83ULL | 0x18ULL;
    return window + off;
}

static int mask_pos(uint32_t m) {
    int p = 0;
    if (!m) return 0;
    while ((m & 1u) == 0) { m >>= 1; p++; }
    return p;
}
static int mask_size(uint32_t m) {
    int n = 0;
    if (!m) return 0;
    while ((m & 1u) == 0) m >>= 1;
    while (m & 1u) { n++; m >>= 1; }
    return n;
}

static int mode_score(uint32_t w, uint32_t h) {
    if (w < 640 || h < 480) return -1;
    if ((uint64_t)w * h * 4u > 12u * 1024u * 1024u) return -1;
    if (w == 1024 && h == 768) return 5000;
    if (w == 1280 && h == 800) return 4900;
    if (w == 1280 && h == 720) return 4800;
    if (w == 1920 && h == 1080) return 4700;
    if (w == 800 && h == 600) return 4600;
    int score = 1000;
    if (w >= 1024 && h >= 768) score += 200;
    return score - (int)(w > 1920 ? w - 1920 : 1920 - w) / 8;
}

static void say(EfiSystemTable *st, const char *s) {
    uint16_t buf[96];
    int i = 0;
    while (s[i] && i < 94) {
        buf[i] = (uint16_t)(uint8_t)s[i];
        i++;
    }
    buf[i] = 0;
    if (st && st->con_out && st->con_out->output_string)
        st->con_out->output_string(st->con_out, buf);
    sputs(s);
}

static uint8_t mapbuf[48 * 1024];
static char fw_vendor_tmp[64];
static uint32_t fw_rev_tmp, gop_mode_tmp;
static uint32_t mem_usable, mem_big_base, mem_big_pages, mem_overlap, mem_high_pages;
static uint32_t ext_kb_final, compact_count, rsdp_saved;
static uint64_t arena_phys, efi_phys, heap_phys;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} CompactEntry;
static CompactEntry compact[128];

static void copy_vendor(uint16_t *src) {
    int i = 0;
    if (!src) {
        fw_vendor_tmp[0] = 0;
        return;
    }
    for (; src[i] && i < 63; i++) {
        uint16_t c = src[i];
        fw_vendor_tmp[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    fw_vendor_tmp[i] = 0;
}

static int mem_type_usable(uint32_t type) {
    return type == 1 || type == 2 || type == 3 || type == 4 || type == 7;
}

static void note_memory(uintn map_size, uintn desc_size, uint32_t arena, uint32_t heap) {
    uint32_t watch[10] = {
        0x6000u, 0x7000u, 0x7C00u, 0x10000u, 0x12000u,
        0x80000u, 0x90000u, arena, 0x1800000u, heap
    };
    uint8_t hit[10];
    for (int i = 0; i < 10; i++) hit[i] = 0;
    mem_usable = 0;
    mem_big_base = 0;
    mem_big_pages = 0;
    mem_high_pages = 0;
    mem_overlap = 0x3FFu;
    if (!desc_size) return;
    for (uintn off = 0; off + sizeof(EfiMemDesc) <= map_size; off += desc_size) {
        EfiMemDesc *d = (EfiMemDesc *)(mapbuf + off);
        int ok = mem_type_usable(d->type);
        uint64_t start = d->physical;
        uint64_t end = start + d->pages * 4096ULL;
        if (ok) {
            mem_usable++;
            if (start >= 0x100000000ULL) {
                uint64_t add = d->pages;
                if (add > 0xFFFFFFFFu - mem_high_pages) mem_high_pages = 0xFFFFFFFFu;
                else mem_high_pages += (uint32_t)add;
                continue;
            }
            uint64_t clip = end > 0x100000000ULL ? 0x100000000ULL : end;
            uint64_t bytes = clip > start ? clip - start : 0;
            if (bytes / 4096ULL > mem_big_pages) {
                mem_big_base = (uint32_t)start;
                mem_big_pages = (uint32_t)(bytes / 4096ULL);
            }
        }
        for (int i = 0; i < 10; i++) {
            uint64_t addr = watch[i];
            if (!addr) continue;
            if (ok && addr >= start && addr < end) hit[i] = 1;
        }
    }
    mem_overlap = 0;
    for (int i = 0; i < 10; i++) {
        if (!watch[i]) continue;
        if (!hit[i]) mem_overlap |= 1u << i;
    }
}

static void sput_hex(uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) sput(hex[(v >> s) & 0xF]);
}

static int try_reserve(EfiBootServices *bs, uint64_t addr, uintn pages) {
    uint64_t got = addr;
    efi_status stt = bs->allocate_pages(2, 2, pages, &got);
    return (stt || got != addr) ? -1 : 0;
}

static void reserve_at(EfiBootServices *bs, EfiSystemTable *st, uint64_t addr, uintn pages, const char *name) {
    if (!try_reserve(bs, addr, pages)) {
        sputs("EFI: reserved ");
        sputs(name);
        sputs("\n");
        return;
    }
    say(st, "GeometricalOS: cannot reserve memory.\r\n");
    sputs("EFI: reserve failed ");
    sputs(name);
    sputs(" ");
    sput_hex(addr);
    sputs("\n");
    for (;;) __asm__ volatile ("hlt");
}

static uint64_t alloc_under4g(EfiBootServices *bs, uintn pages) {
    uint64_t got = 0xFFFFFFFFULL;
    efi_status stt = bs->allocate_pages(1, 2, pages, &got);
    if (stt || !got || got > 0xFFFFF000ULL) return 0;
    return got;
}

static void snapshot_map(uintn map_size, uintn desc_size) {
    compact_count = 0;
    uint64_t mem_bytes = 0;
    if (desc_size) {
        for (uintn off = 0; off + sizeof(EfiMemDesc) <= map_size && off < sizeof(mapbuf); off += desc_size) {
            EfiMemDesc *d = (EfiMemDesc *)(mapbuf + off);
            if (compact_count < 128) {
                compact[compact_count].base = d->physical;
                compact[compact_count].length = d->pages * 4096ULL;
                compact[compact_count].type = d->type;
                compact_count++;
            }
            int usable = d->type == 1 || d->type == 2 || d->type == 3 || d->type == 4 || d->type == 7;
            if (!usable || d->physical < 0x100000ULL || d->physical >= 0x100000000ULL) continue;
            uint64_t end = d->physical + d->pages * 4096ULL;
            if (end > 0x100000000ULL) end = 0x100000000ULL;
            mem_bytes += end - d->physical;
        }
        note_memory(map_size, desc_size, (uint32_t)arena_phys, (uint32_t)heap_phys);
    }
    ext_kb_final = (uint32_t)(mem_bytes / 1024ULL);
    if (ext_kb_final < 65536) ext_kb_final = 65536;
}

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint32_t find_rsdp(EfiSystemTable *st) {
    static uint8_t v2[16] = {
        0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
        0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
    };
    static uint8_t v1[16] = {
        0x30, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11,
        0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D
    };
    if (!st->config || !st->entries) return 0;
    uint8_t *entries = (uint8_t *)st->config;
    uint32_t acpi1 = 0;
    for (uintn i = 0; i < st->entries; i++) {
        uint8_t *g = entries + i * 24u;
        uint64_t ptr = *(uint64_t *)(g + 16);
        if (!ptr || ptr > 0xFFFFF000ULL) continue;
        if (guid_eq(g, v2)) return (uint32_t)ptr;
        if (guid_eq(g, v1)) acpi1 = (uint32_t)ptr;
    }
    return acpi1;
}

static uint32_t save_boot_file(efi_handle image, EfiBootServices *bs, uint8_t *dst) {
    static uint8_t li_guid[16] = {
        0xA1, 0x31, 0x1B, 0x5B, 0x62, 0x95, 0xD2, 0x11,
        0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
    };
    static uint8_t fs_guid[16] = {
        0x22, 0x5B, 0x4E, 0x96, 0x59, 0x64, 0xD2, 0x11,
        0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
    };
    typedef efi_status (*open_fn)(efi_handle, uint8_t *, void **, efi_handle, efi_handle, uint32_t);
    open_fn open = (open_fn)bs->open_protocol;
    void *loaded = 0;
    if (!open || open(image, li_guid, &loaded, image, 0, 2) || !loaded) return 0;
    efi_handle device = *(efi_handle *)((uint8_t *)loaded + 24);
    void *fs = 0;
    if (open(device, fs_guid, &fs, image, 0, 2) || !fs) return 0;
    typedef struct { uint64_t rev; efi_status (*open_volume)(void *, void **); } Fs;
    void *root = 0;
    if (((Fs *)fs)->open_volume(fs, &root) || !root) return 0;
    typedef struct {
        uint64_t rev;
        efi_status (*open)(void *, void **, uint16_t *, uint64_t, uint64_t);
        efi_status (*close)(void *);
        void *del;
        efi_status (*read)(void *, uintn *, void *);
    } File;
    static uint16_t path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\','B','O','O','T','X','6','4','.','E','F','I', 0
    };
    void *file = 0;
    if (((File *)root)->open(root, &file, path, 1, 0) || !file) return 0;
    if (!dst) return 0;
    uint32_t got = 0;
    while (got + 4096u <= 512u * 1024u) {
        uintn n = 4096;
        if (((File *)file)->read(file, &n, dst + got) || !n) break;
        got += (uint32_t)n;
    }
    ((File *)file)->close(file);
    if (got >= 4096) sputs("EFI: saved boot file\n");
    return got >= 4096 ? got : 0;
}

efi_status efi_main(efi_handle image, EfiSystemTable *st) {
    serial_init();
    sputs("\nEFI: GeometricalOS\n");
    fw_rev_tmp = st->firmware_revision;
    copy_vendor(st->firmware_vendor);
    EfiBootServices *bs = st->boot;
    if (bs->set_watchdog) bs->set_watchdog(0, 0, 0, 0);

    EfiGop *gop = 0;
    efi_status stt = bs->locate_protocol(gop_guid, 0, (void **)&gop);
    if (stt || !gop || !gop->mode) {
        say(st, "GeometricalOS: no graphics console.\r\n");
        return 1;
    }

    int best = -1;
    int best_score = -1;
    uint32_t best_w = 0, best_h = 0, best_stride = 0;
    EfiPixelMask best_mask;
    memset(&best_mask, 0, sizeof(best_mask));
    uint32_t modes = gop->mode->max_mode;
    if (modes > 128) modes = 128;
    for (uint32_t i = 0; i < modes; i++) {
        uintn sz = 0;
        EfiGopInfo *info_ptr = 0;
        if (gop->query_mode(gop, i, &sz, &info_ptr) || !info_ptr) continue;
        if (info_ptr->format > 2) continue;
        int score = mode_score(info_ptr->width, info_ptr->height);
        if (score > best_score) {
            best_score = score;
            best = (int)i;
            best_w = info_ptr->width;
            best_h = info_ptr->height;
            best_stride = info_ptr->stride;
            best_mask = info_ptr->mask;
        }
    }
    if (best < 0) {
        say(st, "GeometricalOS: no usable screen mode.\r\n");
        return 2;
    }
    if (gop->set_mode(gop, (uint32_t)best)) {
        say(st, "GeometricalOS: could not set the screen mode.\r\n");
        return 3;
    }
    EfiGopMode *gm = gop->mode;
    if (!gm || !gm->framebuffer || !gm->info) {
        say(st, "GeometricalOS: the display has no framebuffer.\r\n");
        return 4;
    }
    gop_mode_tmp = gm->mode;
    uint32_t width = gm->info->width ? gm->info->width : best_w;
    uint32_t height = gm->info->height ? gm->info->height : best_h;
    uint32_t format = gm->info->format;
    uint32_t stride = gm->info->stride ? gm->info->stride : best_stride;
    EfiPixelMask mask = gm->info->mask.red || format != 2 ? gm->info->mask : best_mask;
    uint64_t fb = gm->framebuffer;
    uint32_t fb_bytes = (uint32_t)gm->framebuffer_size;
    if (!fb_bytes) fb_bytes = stride * height * 4;

    int xbus = 0, xdev = 0, xfn = 0;
    uint64_t xbar = 0;
    uint32_t xsize = 0;
    int have_xhci = find_pci(0x0C, 0x03, 0x30, &xbus, &xdev, &xfn, &xbar, &xsize, 0x20000);
    int nbus = 0, ndev = 0, nfn = 0;
    uint64_t nbar = 0;
    uint32_t nsize = 0;
    int have_nvme = find_pci(0x01, 0x08, 0x02, &nbus, &ndev, &nfn, &nbar, &nsize, 0x4000);

    reserve_at(bs, st, 0x6000, 1, "bootinfo");
    reserve_at(bs, st, 0x7000, 1, "trampoline");
    reserve_at(bs, st, 0x10000, 0x6C, "kernel");
    reserve_at(bs, st, 0x81000, 4, "sound");
    reserve_at(bs, st, 0x7C000, 5, "stack64");
    reserve_at(bs, st, 0x8C000, 5, "stack32");
    if (!try_reserve(bs, 0x1100000, 240)) {
        heap_phys = 0x1100000;
        sputs("EFI: reserved usbheap\n");
    } else {
        heap_phys = alloc_under4g(bs, 240);
        if (!heap_phys) {
            say(st, "GeometricalOS: cannot reserve memory.\r\n");
            sputs("EFI: reserve failed usbheap\n");
            for (;;) __asm__ volatile ("hlt");
        }
        sputs("EFI: relocated usbheap ");
        sput_hex(heap_phys);
        sputs("\n");
    }
    reserve_at(bs, st, 0x1800000, 8, "pages");

    efi_phys = alloc_under4g(bs, 128);
    uint32_t saved_efi = 0;
    if (efi_phys) saved_efi = save_boot_file(image, bs, (uint8_t *)efi_phys);
    arena_phys = alloc_under4g(bs, 3840);
    rsdp_saved = find_rsdp(st);

    int exited = 0;
    uintn map_size, map_key, desc_size;
    uint32_t desc_ver;
    for (int attempt = 0; attempt < 5; attempt++) {
        map_size = sizeof(mapbuf);
        map_key = 0;
        desc_size = 0;
        desc_ver = 0;
        stt = bs->get_memory_map(&map_size, mapbuf, &map_key, &desc_size, &desc_ver);
        if (stt) break;
        snapshot_map(map_size, desc_size);
        stt = bs->exit_boot_services(image, map_key);
        if (!stt) {
            exited = 1;
            break;
        }
    }
    if (!exited) {
        say(st, "GeometricalOS: memory map failed.\r\n");
        sputs("EFI: exit boot services failed\n");
        return 5;
    }

    uint64_t image_bytes = (uint64_t)(_binary_bootimg_bin_end - _binary_bootimg_bin_start);
    if (image_bytes < 8192 || image_bytes > 0x5F000) {
        sputs("EFI: bad kernel image\n");
        for (;;) __asm__ volatile ("hlt");
    }
    memcpy((void *)0x10000, _binary_bootimg_bin_start, image_bytes);

    build_pages();
    if (heap_phys >= 0x2000000ULL && heap_phys < 0x100000000ULL) {
        uint64_t base = heap_phys & ~0x1FFFFFULL;
        uint32_t gi = (uint32_t)((base >> 30) & 3ULL);
        uint32_t idx = (uint32_t)((base >> 21) & 0x1FFULL);
        pd[gi][idx] = base | 0x83ULL | 0x18ULL;
    }
    uint32_t lfb = map_device(fb, fb_bytes, 0xE0000000u);
    uint32_t xhci = 0;
    if (have_xhci) xhci = map_device(xbar, xsize ? xsize : 0x20000u, 0xF0000000u);
    uint32_t nvme = 0;
    if (have_nvme) nvme = map_device(nbar, nsize ? nsize : 0x4000u, 0xF8000000u);

    uint32_t red_pos, red_sz, green_pos, green_sz, blue_pos, blue_sz;
    if (format == 0) {
        red_pos = 0; red_sz = 8; green_pos = 8; green_sz = 8; blue_pos = 16; blue_sz = 8;
    } else if (format == 1) {
        blue_pos = 0; blue_sz = 8; green_pos = 8; green_sz = 8; red_pos = 16; red_sz = 8;
    } else {
        red_pos = (uint32_t)mask_pos(mask.red);
        red_sz = (uint32_t)mask_size(mask.red);
        green_pos = (uint32_t)mask_pos(mask.green);
        green_sz = (uint32_t)mask_size(mask.green);
        blue_pos = (uint32_t)mask_pos(mask.blue);
        blue_sz = (uint32_t)mask_size(mask.blue);
    }

    uint32_t *info = (uint32_t *)0x6000;
    memset(info, 0, 256);
    info[0] = 0xB007DA7Au;
    info[1] = 0;
    info[2] = width;
    info[3] = height;
    info[4] = stride * 4u;
    info[5] = 32;
    info[6] = lfb;
    info[7] = red_sz;
    info[8] = red_pos;
    info[9] = green_sz;
    info[10] = green_pos;
    info[11] = blue_sz;
    info[12] = blue_pos;
    info[13] = ext_kb_final;
    info[14] = 0;
    info[15] = 0;
    info[16] = 0;
    info[17] = xhci;
    info[18] = have_xhci ? (0x80000000u | ((uint32_t)xbus << 16) | ((uint32_t)xdev << 8) | (uint32_t)xfn) : 0;
    info[19] = saved_efi ? (uint32_t)efi_phys : 0;
    info[20] = saved_efi;
    info[21] = nvme;
    info[22] = have_nvme ? (0x80000000u | ((uint32_t)nbus << 16) | ((uint32_t)ndev << 8) | (uint32_t)nfn) : 0;
    info[23] = fw_rev_tmp;
    info[24] = gop_mode_tmp;
    info[25] = mem_usable;
    info[26] = mem_big_base;
    info[27] = mem_big_pages;
    info[28] = mem_overlap;
    for (int i = 0; i < 64; i++) ((char *)(info + 29))[i] = fw_vendor_tmp[i];
    info[45] = mem_high_pages;
    info[46] = 0x6100u;
    info[47] = compact_count;
    info[48] = (uint32_t)arena_phys;
    info[49] = arena_phys ? 3840u : 0;
    info[50] = rsdp_saved;
    info[51] = (uint32_t)heap_phys;
    {
        uint8_t *dst = (uint8_t *)0x6100;
        for (uint32_t i = 0; i < compact_count; i++) {
            memcpy(dst, &compact[i].base, 8);
            memcpy(dst + 8, &compact[i].length, 8);
            memcpy(dst + 16, &compact[i].type, 4);
            dst += 20;
        }
    }

    uint8_t *gdt = (uint8_t *)0x7C00;
    memset(gdt, 0, 64);
    gdt[0] = 23;
    gdt[1] = 0;
    *(uint64_t *)(gdt + 2) = 0x7C10ULL;
    uint8_t *g = (uint8_t *)0x7C10;
    g[8] = 0xFF; g[9] = 0xFF; g[13] = 0x9A; g[14] = 0xCF;
    g[16] = 0xFF; g[17] = 0xFF; g[21] = 0x92; g[22] = 0xCF;

    uint64_t n64 = (uint64_t)(_binary_switch64_bin_end - _binary_switch64_bin_start);
    uint64_t n32 = (uint64_t)(_binary_enter32_bin_end - _binary_enter32_bin_start);
    memcpy((void *)0x7100, _binary_switch64_bin_start, n64);
    memcpy((void *)0x7000, _binary_enter32_bin_start, n32);

    sputs("EFI: jump to kernel\n");
    efi_jump_low(0x1800000ULL);
    for (;;) __asm__ volatile ("hlt");
}
