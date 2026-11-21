#include "nvme.h"
#include "pci.h"
#include "bootinfo.h"
#include "io.h"
#include "serial.h"

#define QSIZE 16

typedef struct __attribute__((packed)) {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t dw10, dw11, dw12, dw13, dw14, dw15;
} NvmeCmd;

typedef struct __attribute__((packed)) {
    uint32_t dw0, dw1;
    uint16_t sqhd, sqid, cid, status;
} NvmeCqe;

_Static_assert(sizeof(NvmeCmd) == 64, "NVMe command must be 64 bytes");
_Static_assert(sizeof(NvmeCqe) == 16, "NVMe completion must be 16 bytes");

static volatile uint8_t *mmio;
static uint32_t stride;
static uint32_t nsid = 1;
static uint32_t lbads = 9;
static int ready;
static NvmeCmd *asq;
static volatile NvmeCqe *acq;
static NvmeCmd *isq;
static volatile NvmeCqe *icq;
static uint8_t *ident;
static uint8_t *bounce;
static uint16_t asq_tail, acq_head, acq_phase = 1;
static uint16_t isq_tail, icq_head, icq_phase = 1;
static uint16_t next_cid = 1;

typedef struct {
    uint64_t start;
    uint32_t sectors;
    int esp;
    int empty;
    int geofs;
} Part;

static Part parts[16];
static int part_count;
static int esp_index = -1;

static uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }
static void w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mmio + off) = v; }
static uint64_t r64(uint32_t off) {
    uint32_t lo = *(volatile uint32_t *)(mmio + off);
    uint32_t hi = *(volatile uint32_t *)(mmio + off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
static void w64(uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(mmio + off) = (uint32_t)v;
    *(volatile uint32_t *)(mmio + off + 4) = (uint32_t)(v >> 32);
}

static void doorbell(int index, uint32_t value) {
    volatile uint8_t *reg = mmio + 0x1000u + (uint32_t)index * stride;
    *(volatile uint32_t *)reg = value;
}

static uint16_t last_status;

static int wait_cq(volatile NvmeCqe *cq, uint16_t *head, uint16_t *phase) {
    for (int i = 0; i < 200000; i++) {
        uint16_t status = cq[*head].status;
        if ((status & 1u) != *phase) continue;
        last_status = status;
        int code = (status >> 1) & 0xFF;
        *head = (uint16_t)((*head + 1) % QSIZE);
        if (*head == 0) *phase ^= 1u;
        return code == 0 ? 0 : -1;
    }
    last_status = 0xFFFF;
    return -2;
}

static int admin(uint8_t opcode, uint32_t ns, uint64_t prp, uint32_t dw10, uint32_t dw11) {
    NvmeCmd *c = &asq[asq_tail];
    memset(c, 0, sizeof(*c));
    c->cdw0 = opcode | ((uint32_t)next_cid++ << 16);
    c->nsid = ns;
    c->prp1 = prp;
    c->dw10 = dw10;
    c->dw11 = dw11;
    __asm__ volatile ("mfence" ::: "memory");
    asq_tail = (uint16_t)((asq_tail + 1) % QSIZE);
    doorbell(0, asq_tail);
    int rc = wait_cq(acq, &acq_head, &acq_phase);
    doorbell(1, acq_head);
    return rc;
}

static int io_cmd(uint8_t opcode, uint64_t lba, uint32_t nlb, uint64_t prp) {
    NvmeCmd *c = &isq[isq_tail];
    memset(c, 0, sizeof(*c));
    c->cdw0 = opcode | ((uint32_t)next_cid++ << 16);
    c->nsid = nsid;
    c->prp1 = prp;
    c->dw10 = (uint32_t)lba;
    c->dw11 = (uint32_t)(lba >> 32);
    c->dw12 = nlb;
    __asm__ volatile ("mfence" ::: "memory");
    isq_tail = (uint16_t)((isq_tail + 1) % QSIZE);
    doorbell(2, isq_tail);
    int rc = wait_cq(icq, &icq_head, &icq_phase);
    doorbell(3, icq_head);
    return rc;
}

static int enable_controller(void) {
    uint64_t cap = r64(0);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
    if (mpsmin > 0) return -1;
    stride = 4u << (uint32_t)((cap >> 32) & 0xF);
    uint32_t cc = r32(0x14);
    if ((cc & 1u) || (r32(0x1C) & 1u)) {
        w32(0x14, cc & ~1u);
        for (int i = 0; i < 1000000; i++) if ((r32(0x1C) & 1u) == 0) break;
        if (r32(0x1C) & 1u) return -2;
    }
    asq = (NvmeCmd *)0x1300000;
    acq = (volatile NvmeCqe *)0x1301000;
    isq = (NvmeCmd *)0x1302000;
    icq = (volatile NvmeCqe *)0x1303000;
    ident = (uint8_t *)0x1304000;
    bounce = (uint8_t *)0x1305000;
    memset(asq, 0, 4096);
    memset((void *)acq, 0, 4096);
    memset(isq, 0, 4096);
    memset((void *)icq, 0, 4096);
    w32(0x24, ((QSIZE - 1) << 16) | (QSIZE - 1));
    w64(0x28, (uint64_t)(uintptr_t)asq);
    w64(0x30, (uint64_t)(uintptr_t)acq);
    w32(0x14, 1u | (0u << 4) | (0u << 7) | (6u << 16) | (4u << 20));
    for (int i = 0; i < 1000000; i++) if (r32(0x1C) & 1u) break;
    if ((r32(0x1C) & 1u) == 0) return -3;
    asq_tail = acq_head = isq_tail = icq_head = 0;
    acq_phase = icq_phase = 1;
    if (admin(0x06, 0, (uint64_t)(uintptr_t)ident, 1, 0)) return -4;
    uint32_t nn = *(uint32_t *)(ident + 516);
    if (!nn) return -5;
    nsid = 1;
    memset(ident, 0, 4096);
    if (admin(0x06, nsid, (uint64_t)(uintptr_t)ident, 0, 0)) return -6;
    uint8_t flbas = ident[26] & 0x0F;
    lbads = ident[128 + flbas * 4 + 2];
    if (lbads != 9 && lbads != 12) return -7;
    if (admin(0x05, 0, (uint64_t)(uintptr_t)icq, ((QSIZE - 1) << 16) | 1, 1)) return -8;
    if (admin(0x01, 0, (uint64_t)(uintptr_t)isq, ((QSIZE - 1) << 16) | 1, (1u << 16) | 1u)) return -9;
    serial_write("NVME: ready lbads ");
    serial_dec(lbads);
    serial_write("\n");
    return 0;
}

static int controller_at(uint32_t bar, int bus, int dev, int fn) {
    uint16_t cmd = (uint16_t)pci_read(bus, dev, fn, 4);
    pci_write16(bus, dev, fn, 4, (uint16_t)(cmd | 0x06));
    mmio = (volatile uint8_t *)(uintptr_t)bar;
    serial_write("NVME: mmio ");
    serial_hex32(bar);
    serial_write("\n");
    int rc = enable_controller();
    if (rc) {
        serial_write("NVME: enable ");
        serial_dec((uint32_t)(-rc));
        serial_write(" status ");
        serial_hex32(last_status);
        serial_write("\n");
    }
    ready = rc == 0;
    return ready ? 0 : -1;
}

int nvme_init(void) {
    if (ready) return 0;
    const BootInfo *bi = bootinfo();
    if (bi->magic == BOOTINFO_MAGIC && bi->nvme_mmio && (bi->nvme_pci & 0x80000000u)) {
        int bus = (int)((bi->nvme_pci >> 16) & 0xFF);
        int dev = (int)((bi->nvme_pci >> 8) & 0x1F);
        int fn = (int)(bi->nvme_pci & 7);
        if (controller_at(bi->nvme_mmio, bus, dev, fn) == 0) return 0;
    }
    int bus, dev, fn;
    if (pci_find_class(0x01, 0x08, 0x02, &bus, &dev, &fn)) {
        serial_write("NVME: none\n");
        return -1;
    }
    uint32_t lo = pci_read(bus, dev, fn, 0x10);
    uint32_t hi = ((lo & 7u) == 4u) ? pci_read(bus, dev, fn, 0x14) : 0;
    if ((lo & 1u) || hi) {
        serial_write("NVME: BAR out of reach\n");
        return -1;
    }
    return controller_at(lo & ~0xFu, bus, dev, fn);
}

int nvme_read(uint64_t lba, void *buf, uint32_t count) {
    if (!ready && nvme_init()) return -1;
    uint8_t *out = buf;
    if (lbads == 9) {
        while (count) {
            if (io_cmd(0x02, lba, 0, (uint64_t)(uintptr_t)bounce)) return -1;
            memcpy(out, bounce, 512);
            out += 512;
            lba++;
            count--;
        }
        return 0;
    }
    while (count) {
        uint64_t block = lba >> 3;
        uint32_t skip = (uint32_t)(lba & 7u);
        uint32_t chunk = 8u - skip;
        if (chunk > count) chunk = count;
        if (io_cmd(0x02, block, 0, (uint64_t)(uintptr_t)bounce)) return -1;
        memcpy(out, bounce + skip * 512u, chunk * 512u);
        out += chunk * 512u;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int nvme_write(uint64_t lba, const void *buf, uint32_t count) {
    if (!ready && nvme_init()) return -1;
    const uint8_t *in = buf;
    if (lbads == 9) {
        while (count) {
            memcpy(bounce, in, 512);
            if (io_cmd(0x01, lba, 0, (uint64_t)(uintptr_t)bounce)) return -1;
            in += 512;
            lba++;
            count--;
        }
        return 0;
    }
    while (count) {
        uint64_t block = lba >> 3;
        uint32_t skip = (uint32_t)(lba & 7u);
        uint32_t chunk = 8u - skip;
        if (chunk > count) chunk = count;
        if (io_cmd(0x02, block, 0, (uint64_t)(uintptr_t)bounce)) return -1;
        memcpy(bounce + skip * 512u, in, chunk * 512u);
        if (io_cmd(0x01, block, 0, (uint64_t)(uintptr_t)bounce)) return -1;
        in += chunk * 512u;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static const uint8_t esp_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
static const uint8_t msr_guid[16] = {
    0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
    0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE
};

static int sector_empty(uint64_t lba) {
    if (nvme_read(lba, bounce, 1)) return 0;
    for (int i = 0; i < 512; i++) if (bounce[i]) return 0;
    return 1;
}

static void scan_gpt(void) {
    part_count = 0;
    esp_index = -1;
    if (nvme_read(1, bounce, 1)) return;
    if (memcmp(bounce, "EFI PART", 8) != 0) return;
    uint64_t entry_lba = *(uint64_t *)(bounce + 72);
    uint32_t count = *(uint32_t *)(bounce + 80);
    uint32_t esize = *(uint32_t *)(bounce + 84);
    if (esize < 128 || count > 128) return;
    uint8_t *entries = (uint8_t *)0x1306000;
    uint32_t bytes = count * esize;
    uint32_t sectors = (bytes + 511u) / 512u;
    if (sectors > 32) return;
    if (nvme_read(entry_lba, entries, sectors)) return;
    for (uint32_t i = 0; i < count && part_count < 16; i++) {
        uint8_t *e = entries + i * esize;
        int used = 0;
        for (int b = 0; b < 16; b++) if (e[b]) used = 1;
        if (!used) continue;
        uint64_t start = *(uint64_t *)(e + 32);
        uint64_t end = *(uint64_t *)(e + 40);
        if (end < start) continue;
        uint64_t n64 = end - start + 1;
        if (n64 > 0xFFFFFFFFu) n64 = 0xFFFFFFFFu;
        Part *p = &parts[part_count];
        memset(p, 0, sizeof(*p));
        p->start = start;
        p->sectors = (uint32_t)n64;
        p->esp = guid_eq(e, esp_guid);
        if (p->esp) esp_index = part_count;
        int reserved = p->esp || guid_eq(e, msr_guid);
        if (!reserved && p->sectors >= 65536u && p->sectors <= 33554432u) {
            int blank = 1;
            for (uint32_t s = 0; s < 8; s++) if (!sector_empty(start + s)) { blank = 0; break; }
            p->empty = blank;
        }
        if (!reserved && p->sectors > 2048) {
            if (nvme_read(start + 2048, bounce, 1) == 0 && memcmp(bounce, "GEOFS001", 8) == 0)
                p->geofs = 1;
        }
        if (p->empty) {
            serial_write("NVME: empty MB ");
            serial_dec(p->sectors / 2048u);
            serial_write("\n");
        }
        part_count++;
    }
    serial_write("NVME: partitions ");
    serial_dec((uint32_t)part_count);
    serial_write("\n");
}

int nvme_find_geofs(uint64_t *start, uint32_t *sectors) {
    if (nvme_init()) return 0;
    if (!part_count) scan_gpt();
    for (int i = 0; i < part_count; i++) {
        if (!parts[i].geofs) continue;
        *start = parts[i].start;
        *sectors = parts[i].sectors;
        return 1;
    }
    return 0;
}

int nvme_find_empty(uint64_t *start, uint32_t *sectors) {
    if (nvme_init()) return 0;
    if (!part_count) scan_gpt();
    for (int i = 0; i < part_count; i++) {
        if (!parts[i].empty || parts[i].geofs) continue;
        *start = parts[i].start;
        *sectors = parts[i].sectors;
        return 1;
    }
    return 0;
}

static uint32_t fat_sectors, reserved, fat_size, cluster_sectors, root_cluster;
static uint32_t *fat;
static uint64_t esp_lba;

static uint32_t fat_next(uint32_t c) { return fat[c] & 0x0FFFFFFFu; }
static int fat_eoc(uint32_t c) { return c >= 0x0FFFFFF8u; }

static int esp_read(uint32_t off, void *buf, uint32_t n) {
    return nvme_read(esp_lba + off, buf, n);
}
static int esp_write(uint32_t off, const void *buf, uint32_t n) {
    return nvme_write(esp_lba + off, buf, n);
}

static uint32_t cluster_lba(uint32_t cluster) {
    return reserved + fat_sectors * fat_size + (cluster - 2u) * cluster_sectors;
}

static int load_fat(void) {
    if (fat_size > 4096) return -1;
    fat = (uint32_t *)0x1400000;
    return esp_read(reserved, fat, fat_size);
}

static int store_fat(void) {
    for (uint32_t copy = 0; copy < fat_sectors; copy++)
        if (esp_write(reserved + copy * fat_size, fat, fat_size)) return -1;
    return 0;
}

static uint32_t alloc_cluster(void) {
    for (uint32_t i = 2; i < fat_size * 128u; i++) {
        if (fat[i] == 0) {
            fat[i] = 0x0FFFFFFFu;
            return i;
        }
    }
    return 0;
}

static int dir_find(uint32_t cluster, const char *name11, uint32_t *out_cluster, uint32_t *entry_lba, uint32_t *entry_off) {
    while (cluster >= 2 && !fat_eoc(cluster)) {
        uint32_t lba = cluster_lba(cluster);
        for (uint32_t s = 0; s < cluster_sectors; s++) {
            if (esp_read(lba + s, bounce, 1)) return -1;
            for (int i = 0; i < 512; i += 32) {
                if (bounce[i] == 0x00) return 0;
                if (bounce[i] == 0xE5) continue;
                if ((bounce[i + 11] & 0x08)) continue;
                int match = 1;
                for (int k = 0; k < 11; k++) if (bounce[i + k] != (uint8_t)name11[k]) match = 0;
                if (!match) continue;
                uint32_t hi = *(uint16_t *)(bounce + i + 20);
                uint32_t lo = *(uint16_t *)(bounce + i + 26);
                if (out_cluster) *out_cluster = (hi << 16) | lo;
                if (entry_lba) *entry_lba = lba + s;
                if (entry_off) *entry_off = (uint32_t)i;
                return 1;
            }
        }
        cluster = fat_next(cluster);
    }
    return 0;
}

static int dir_create(uint32_t cluster, const char *name11, uint8_t attr, uint32_t file_cluster, uint32_t size) {
    while (cluster >= 2 && !fat_eoc(cluster)) {
        uint32_t lba = cluster_lba(cluster);
        for (uint32_t s = 0; s < cluster_sectors; s++) {
            if (esp_read(lba + s, bounce, 1)) return -1;
            for (int i = 0; i < 512; i += 32) {
                if (bounce[i] != 0x00 && bounce[i] != 0xE5) continue;
                memset(bounce + i, 0, 32);
                memcpy(bounce + i, name11, 11);
                bounce[i + 11] = attr;
                *(uint16_t *)(bounce + i + 20) = (uint16_t)(file_cluster >> 16);
                *(uint16_t *)(bounce + i + 26) = (uint16_t)file_cluster;
                *(uint32_t *)(bounce + i + 28) = size;
                return esp_write(lba + s, bounce, 1);
            }
        }
        uint32_t next = fat_next(cluster);
        if (fat_eoc(next)) return -1;
        cluster = next;
    }
    return -1;
}

int nvme_store_bootloader(void) {
    const BootInfo *bi = bootinfo();
    if (!bi->efi_file_addr || bi->efi_file_size < 4096 || bi->efi_file_size > 512u * 1024u)
        return -1;
    if (nvme_init()) return -1;
    if (!part_count) scan_gpt();
    if (esp_index < 0) return -2;
    esp_lba = parts[esp_index].start;
    if (esp_read(0, bounce, 1)) return -3;
    if (bounce[510] != 0x55 || bounce[511] != 0xAA) return -3;
    if (*(uint16_t *)(bounce + 11) != 512) return -3;
    if (memcmp(bounce + 82, "FAT32", 5) != 0) return -3;
    cluster_sectors = bounce[13];
    reserved = *(uint16_t *)(bounce + 14);
    fat_sectors = bounce[16];
    fat_size = *(uint32_t *)(bounce + 36);
    root_cluster = *(uint32_t *)(bounce + 44);
    if (cluster_sectors == 0 || cluster_sectors > 64 || fat_sectors < 1 || fat_sectors > 2)
        return -3;
    if (load_fat()) return -4;
    uint32_t efi_dir = 0;
    int found = dir_find(root_cluster, "EFI        ", &efi_dir, 0, 0);
    if (found != 1 || efi_dir < 2) return -5;
    uint32_t geom_dir = 0, entry_lba = 0, entry_off = 0;
    found = dir_find(efi_dir, "GEOMOS     ", &geom_dir, &entry_lba, &entry_off);
    if (found < 0) return -6;
    if (found == 0) {
        geom_dir = alloc_cluster();
        if (!geom_dir) return -7;
        memset(bounce, 0, 512);
        for (uint32_t s = 0; s < cluster_sectors; s++)
            if (esp_write(cluster_lba(geom_dir) + s, bounce, 1)) return -7;
        if (dir_create(efi_dir, "GEOMOS     ", 0x10, geom_dir, 0)) return -7;
    }
    uint32_t old = 0;
    found = dir_find(geom_dir, "BOOTX64 EFI", &old, &entry_lba, &entry_off);
    if (found < 0) return -8;
    if (found == 1) {
        uint32_t c = old;
        while (c >= 2 && !fat_eoc(c)) {
            uint32_t n = fat_next(c);
            fat[c] = 0;
            c = n;
        }
    }
    uint32_t bytes_left = bi->efi_file_size;
    const uint8_t *src = (const uint8_t *)(uintptr_t)bi->efi_file_addr;
    uint32_t first = 0, prev = 0;
    while (bytes_left) {
        uint32_t c = alloc_cluster();
        if (!c) return -9;
        if (!first) first = c;
        if (prev) fat[prev] = c;
        uint32_t chunk = cluster_sectors * 512u;
        if (chunk > bytes_left) chunk = bytes_left;
        uint32_t lba = cluster_lba(c);
        for (uint32_t s = 0; s < cluster_sectors; s++) {
            memset(bounce, 0, 512);
            uint32_t n = 0;
            if (bytes_left) {
                n = bytes_left > 512 ? 512 : bytes_left;
                memcpy(bounce, src, n);
                src += n;
                bytes_left -= n;
            }
            if (esp_write(lba + s, bounce, 1)) return -9;
            (void)n;
        }
        prev = c;
    }
    if (store_fat()) return -10;
    if (found == 1) {
        if (esp_read(entry_lba, bounce, 1)) return -11;
        *(uint16_t *)(bounce + entry_off + 20) = (uint16_t)(first >> 16);
        *(uint16_t *)(bounce + entry_off + 26) = (uint16_t)first;
        *(uint32_t *)(bounce + entry_off + 28) = bi->efi_file_size;
        if (esp_write(entry_lba, bounce, 1)) return -11;
    } else if (dir_create(geom_dir, "BOOTX64 EFI", 0x20, first, bi->efi_file_size)) {
        return -11;
    }
    serial_write("NVME: boot file stored\n");
    return 0;
}
