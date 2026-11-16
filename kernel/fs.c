#include "fs.h"
#include "ata.h"
#include "io.h"
#include "serial.h"

#define FS_LBA           2048u
#define INODE_COUNT      160
#define BITMAP_SECTORS   32u
#define INODE_SECTORS    ((INODE_COUNT * 64) / 512)
#define DATA_OFF         (1u + BITMAP_SECTORS + INODE_SECTORS)
#define DATA_LBA         (FS_LBA + DATA_OFF)
#define BITMAP_LBA       (FS_LBA + 1u)
#define INODE_LBA        (FS_LBA + 1u + BITMAP_SECTORS)

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t total_sectors;
    uint32_t data_blocks;
    uint32_t inode_count;
    uint32_t reserved[4];
} Super;

typedef struct __attribute__((packed)) {
    char name[28];
    uint8_t type;
    uint8_t reserved;
    uint16_t parent;
    uint32_t size;
    uint32_t start_lba;
    uint32_t blocks;
    uint32_t unused[5];
} Inode;

_Static_assert(sizeof(Inode) == 64, "GeoFS inode must be 64 bytes");
_Static_assert(sizeof(Super) <= 512, "GeoFS superblock must fit in one sector");

static Super *super = (Super *)0x640000;
static Inode *inodes = (Inode *)0x641000;
static uint8_t *bitmap = (uint8_t *)0x650000;
static uint8_t *copy_buffer = (uint8_t *)0x740000;
static int mounted;

static int bit_get(uint32_t i) {
    return (bitmap[i >> 3] >> (i & 7)) & 1;
}
static void bit_set(uint32_t i, int v) {
    uint8_t mask = (uint8_t)(1u << (i & 7));
    if (v) bitmap[i >> 3] |= mask;
    else bitmap[i >> 3] &= (uint8_t)~mask;
}

static int valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    int n = 0;
    for (const char *p = name; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return n > 0 && n <= 27;
}

int fs_sync(void) {
    if (ata_write(FS_LBA, super, 1)) return -1;
    if (ata_write(BITMAP_LBA, bitmap, BITMAP_SECTORS)) return -1;
    if (ata_write(INODE_LBA, inodes, INODE_SECTORS)) return -1;
    return 0;
}

int fs_format(uint32_t disk_sectors) {
    memset(super, 0, 512);
    memset(bitmap, 0, BITMAP_SECTORS * 512);
    memset(inodes, 0, INODE_SECTORS * 512);
    memcpy(super->magic, "GEOFS001", 8);
    super->total_sectors = disk_sectors;
    super->inode_count = INODE_COUNT;
    uint32_t maxb = BITMAP_SECTORS * 512u * 8u;
    uint32_t avail = (disk_sectors > DATA_LBA) ? (disk_sectors - DATA_LBA) : 0;
    if (avail > maxb) avail = maxb;
    super->data_blocks = avail;
    inodes[0].type = 2;
    inodes[0].name[0] = 0;
    inodes[0].parent = 0;
    mounted = 1;
    if (fs_sync()) return -1;
    serial_write("FS: formatted blocks ");
    serial_dec(avail);
    serial_write("\n");
    return 0;
}

int fs_mount(void) {
    if (ata_read(FS_LBA, super, 1)) return -1;
    if (memcmp(super->magic, "GEOFS001", 8) != 0) return -2;
    if (super->inode_count != INODE_COUNT || super->data_blocks == 0 ||
        super->data_blocks > BITMAP_SECTORS * 512u * 8u)
        return -3;
    if (ata_read(BITMAP_LBA, bitmap, BITMAP_SECTORS)) return -1;
    if (ata_read(INODE_LBA, inodes, INODE_SECTORS)) return -1;
    if (inodes[0].type != 2) return -4;
    mounted = 1;
    return 0;
}

static int alloc_inode(void) {
    for (int i = 1; i < INODE_COUNT; i++) {
        if (inodes[i].type == 0) return i;
    }
    return -1;
}

static int alloc_run(uint32_t n, uint32_t *lba_out) {
    if (n == 0) return 0;
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0; i < super->data_blocks; i++) {
        if (!bit_get(i)) {
            if (run == 0) start = i;
            run++;
            if (run == n) {
                for (uint32_t j = 0; j < n; j++) bit_set(start + j, 1);
                *lba_out = DATA_LBA + start;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

static void free_run(uint32_t lba, uint32_t n) {
    if (n == 0 || lba < DATA_LBA) return;
    uint32_t start = lba - DATA_LBA;
    for (uint32_t j = 0; j < n; j++) {
        if (start + j < super->data_blocks) bit_set(start + j, 0);
    }
}

int fs_lookup(uint16_t parent, const char *name) {
    if (!mounted || !name) return -1;
    for (int i = 1; i < INODE_COUNT; i++) {
        if (!inodes[i].type) continue;
        if (inodes[i].parent != parent) continue;
        if (strcasecmp(inodes[i].name, name) == 0) return i;
    }
    return -1;
}

int fs_create_dir(uint16_t parent, const char *name) {
    if (!valid_name(name)) return -1;
    int existing = fs_lookup(parent, name);
    if (existing >= 0) return inodes[existing].type == 2 ? existing : -2;
    int idx = alloc_inode();
    if (idx < 0) return -3;
    Inode *ino = &inodes[idx];
    memset(ino, 0, sizeof(*ino));
    strncpy(ino->name, name, 28);
    ino->type = 2;
    ino->parent = parent;
    if (fs_sync()) return -4;
    return idx;
}

int fs_write(uint16_t parent, const char *name, const void *data, uint32_t size) {
    if (!mounted || !valid_name(name)) return -1;
    if (size > FS_MAX_FILE_BYTES) return -2;
    int idx = fs_lookup(parent, name);
    if (idx >= 0 && inodes[idx].type != 1) return -3;
    int newly_created = 0;
    if (idx < 0) {
        idx = alloc_inode();
        if (idx < 0) return -4;
        memset(&inodes[idx], 0, sizeof(Inode));
        strncpy(inodes[idx].name, name, 28);
        inodes[idx].type = 1;
        inodes[idx].parent = parent;
        newly_created = 1;
    }
    Inode *ino = &inodes[idx];
    if (ino->blocks) {
        free_run(ino->start_lba, ino->blocks);
        ino->blocks = 0;
        ino->start_lba = 0;
    }
    uint32_t blocks = (size + 511u) / 512u;
    uint32_t lba = 0;
    if (blocks) {
        if (alloc_run(blocks, &lba)) {
            if (newly_created) {
                memset(ino, 0, sizeof(*ino));
                fs_sync();
            }
            return -5;
        }
        const uint8_t *src = data;
        uint32_t full_blocks = size / 512u;
        uint16_t batch[16 * 256];
        for (uint32_t s = 0; s < full_blocks;) {
            uint32_t count = full_blocks - s;
            if (count > 16) count = 16;
            memcpy(batch, src + s * 512u, count * 512u);
            if (ata_write(lba + s, batch, count)) {
                free_run(lba, blocks);
                if (newly_created) {
                    memset(ino, 0, sizeof(*ino));
                    fs_sync();
                }
                return -6;
            }
            s += count;
        }
        if (size & 511u) {
            uint8_t sec[512];
            uint32_t off = full_blocks * 512u;
            uint32_t chunk = size - off;
            memset(sec, 0, sizeof(sec));
            memcpy(sec, src + off, chunk);
            if (ata_write(lba + full_blocks, sec, 1)) {
                free_run(lba, blocks);
                if (newly_created) {
                    memset(ino, 0, sizeof(*ino));
                    fs_sync();
                }
                return -6;
            }
        }
    }
    ino->start_lba = lba;
    ino->blocks = blocks;
    ino->size = size;
    ino->type = 1;
    if (fs_sync()) return -7;
    return idx;
}

int fs_read(int inode, void *buf, uint32_t max, uint32_t *out_size) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT) return -1;
    Inode *ino = &inodes[inode];
    if (ino->type != 1) return -2;
    uint32_t size = ino->size;
    if (size > max) size = max;
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < size) {
        uint8_t sec[512];
        if (ata_read(ino->start_lba + (done / 512u), sec, 1)) return -3;
        uint32_t off = done % 512u;
        uint32_t chunk = 512u - off;
        if (chunk > size - done) chunk = size - done;
        memcpy(dst + done, sec + off, chunk);
        done += chunk;
    }
    if (out_size) *out_size = size;
    if (size < max) dst[size] = 0;
    return 0;
}

static void delete_inode_memory(int inode) {
    if (inodes[inode].type == 1 && inodes[inode].blocks)
        free_run(inodes[inode].start_lba, inodes[inode].blocks);
    memset(&inodes[inode], 0, sizeof(Inode));
}

int fs_delete(int inode) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT) return -1;
    if (inodes[inode].type == 2) {
        int kids[1];
        if (fs_list((uint16_t)inode, kids, 1) > 0) return -2;
    }
    delete_inode_memory(inode);
    return fs_sync();
}

int fs_recycle_bin(void) {
    int id = fs_lookup(FS_ROOT, "Recycle Bin");
    if (id >= 0) return inodes[id].type == 2 ? id : -1;
    return fs_create_dir(FS_ROOT, "Recycle Bin");
}

int fs_recycle(int inode) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT ||
        !inodes[inode].type || inode == fs_lookup(FS_ROOT, "Recycle Bin"))
        return -1;
    int bin = fs_recycle_bin();
    if (bin < 0 || inodes[inode].parent == (uint16_t)bin) return -1;
    inodes[inode].unused[0] = inodes[inode].parent;
    inodes[inode].parent = (uint16_t)bin;
    return fs_sync();
}

int fs_restore(int inode) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT || !inodes[inode].type)
        return -1;
    int bin = fs_lookup(FS_ROOT, "Recycle Bin");
    if (bin < 0 || inodes[inode].parent != (uint16_t)bin) return -1;
    uint16_t parent = (uint16_t)inodes[inode].unused[0];
    if (parent >= INODE_COUNT || inodes[parent].type != 2) parent = FS_ROOT;
    if (fs_lookup(parent, inodes[inode].name) >= 0) return -2;
    inodes[inode].parent = parent;
    inodes[inode].unused[0] = 0;
    return fs_sync();
}

static int inside_directory(int inode, uint16_t dir) {
    uint16_t parent = inodes[inode].parent;
    for (int depth = 0; depth < INODE_COUNT && parent != FS_ROOT; depth++) {
        if (parent == dir) return 1;
        if (parent >= INODE_COUNT || inodes[parent].type != 2) return 0;
        parent = inodes[parent].parent;
    }
    return 0;
}

int fs_delete_tree(int inode) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT || !inodes[inode].type)
        return -1;
    int bin = fs_lookup(FS_ROOT, "Recycle Bin");
    if (inode == bin) return -1;

    for (int pass = 0; pass < INODE_COUNT; pass++) {
        int progress = 0;
        for (int i = 1; i < INODE_COUNT; i++) {
            if (!inodes[i].type || !inside_directory(i, (uint16_t)inode)) continue;
            if (inodes[i].type == 2) {
                int child[1];
                if (fs_list((uint16_t)i, child, 1) > 0) continue;
            }
            delete_inode_memory(i);
            progress = 1;
        }
        int child[1];
        if (fs_list((uint16_t)inode, child, 1) == 0) {
            delete_inode_memory(inode);
            return fs_sync();
        }
        if (!progress) {
            fs_sync();
            return -1;
        }
    }
    fs_sync();
    return -1;
}

int fs_rename(int inode, const char *name) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT ||
        !inodes[inode].type || !valid_name(name))
        return -1;
    if (inode == fs_lookup(FS_ROOT, "Recycle Bin")) return -1;
    int existing = fs_lookup(inodes[inode].parent, name);
    if (existing >= 0 && existing != inode) return -2;
    char old_name[28];
    memcpy(old_name, inodes[inode].name, sizeof(old_name));
    memset(inodes[inode].name, 0, sizeof(inodes[inode].name));
    strncpy(inodes[inode].name, name, sizeof(inodes[inode].name));
    if (fs_sync()) {
        memcpy(inodes[inode].name, old_name, sizeof(old_name));
        fs_sync();
        return -3;
    }
    return 0;
}

int fs_move(int inode, uint16_t parent) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT ||
        !inodes[inode].type || parent >= INODE_COUNT ||
        inodes[parent].type != 2)
        return -1;
    int bin = fs_lookup(FS_ROOT, "Recycle Bin");
    if (inode == bin || (bin >= 0 &&
        (parent == (uint16_t)bin || inodes[inode].parent == (uint16_t)bin)))
        return -1;
    if (inodes[inode].parent == parent) return 0;
    if (inode == parent) return -2;
    if (inside_directory(parent, (uint16_t)inode)) return -2;
    if (fs_lookup(parent, inodes[inode].name) >= 0) return -3;
    uint16_t old_parent = inodes[inode].parent;
    inodes[inode].parent = parent;
    if (fs_sync()) {
        inodes[inode].parent = old_parent;
        fs_sync();
        return -4;
    }
    return 0;
}

static int copy_file_as(int source, uint16_t parent, const char *name) {
    uint32_t size = fs_size(source), read_size = 0;
    if (fs_read(source, copy_buffer, FS_MAX_FILE_BYTES, &read_size) ||
        read_size != size)
        return -1;
    int copied = fs_write(parent, name, copy_buffer, size);
    if (copied < 0) {
        int partial = fs_lookup(parent, name);
        if (partial >= 0 && !fs_is_dir(partial)) fs_delete(partial);
        return -1;
    }
    return copied;
}

static int copy_file(int source, uint16_t parent) {
    return copy_file_as(source, parent, fs_name(source));
}

int fs_copy_as(int inode, uint16_t parent, const char *name) {
    if (!mounted || inode <= 0 || inode >= INODE_COUNT ||
        !inodes[inode].type || parent >= INODE_COUNT ||
        inodes[parent].type != 2 || !valid_name(name))
        return -1;
    if (fs_lookup(parent, name) >= 0) return -2;
    if (inodes[inode].type == 1)
        return copy_file_as(inode, parent, name) < 0 ? -3 : 0;
    if (inode == parent || inside_directory(parent, (uint16_t)inode)) return -4;

    int copy_root = fs_create_dir(parent, name);
    if (copy_root < 0) return -5;
    uint16_t source_dirs[INODE_COUNT], target_dirs[INODE_COUNT];
    int read = 0, write = 1;
    source_dirs[0] = (uint16_t)inode;
    target_dirs[0] = (uint16_t)copy_root;

    while (read < write) {
        int source = source_dirs[read];
        uint16_t target = target_dirs[read++];
        int children[INODE_COUNT];
        int count = fs_list((uint16_t)source, children, INODE_COUNT);
        for (int i = 0; i < count; i++) {
            int child = children[i];
            if (fs_is_dir(child)) {
                if (write >= INODE_COUNT) goto copy_failed;
                int created = fs_create_dir(target, fs_name(child));
                if (created < 0) goto copy_failed;
                source_dirs[write] = (uint16_t)child;
                target_dirs[write++] = (uint16_t)created;
            } else if (copy_file(child, target) < 0) {
                goto copy_failed;
            }
        }
    }
    return 0;

copy_failed:
    fs_delete_tree(copy_root);
    return -6;
}

int fs_copy(int inode, uint16_t parent) {
    if (inode <= 0 || inode >= INODE_COUNT) return -1;
    return fs_copy_as(inode, parent, fs_name(inode));
}

int fs_restore_tree(int source_dir, uint16_t dest_dir) {
    if (!mounted || source_dir <= 0 || source_dir >= INODE_COUNT ||
        inodes[source_dir].type != 2 || dest_dir >= INODE_COUNT ||
        inodes[dest_dir].type != 2)
        return -1;
    if (source_dir == dest_dir ||
        inside_directory(dest_dir, (uint16_t)source_dir))
        return -6;
    uint16_t source_dirs[INODE_COUNT], target_dirs[INODE_COUNT];
    int read = 0, write = 1;
    source_dirs[0] = (uint16_t)source_dir;
    target_dirs[0] = dest_dir;

    while (read < write) {
        int source = source_dirs[read];
        uint16_t target = target_dirs[read++];
        int children[INODE_COUNT];
        int count = fs_list((uint16_t)source, children, INODE_COUNT);
        for (int i = 0; i < count; i++) {
            int child = children[i];
            int existing = fs_lookup(target, fs_name(child));
            if (fs_is_dir(child)) {
                uint16_t target_child;
                if (existing >= 0) {
                    if (!fs_is_dir(existing)) return -2;
                    target_child = (uint16_t)existing;
                } else {
                    int created = fs_create_dir(target, fs_name(child));
                    if (created < 0) return -3;
                    target_child = (uint16_t)created;
                }
                if (write >= INODE_COUNT) return -4;
                source_dirs[write] = (uint16_t)child;
                target_dirs[write++] = target_child;
            } else {
                if (existing >= 0 && fs_is_dir(existing)) return -2;
                uint32_t size = fs_size(child), read_size = 0;
                if (fs_read(child, copy_buffer, FS_MAX_FILE_BYTES, &read_size) ||
                    read_size != size ||
                    fs_write(target, fs_name(child), copy_buffer, size) < 0)
                    return -5;
            }
        }
    }
    return 0;
}

int fs_get_space(FsSpaceInfo *info) {
    if (!mounted || !info) return -1;
    uint32_t used = 0;
    for (uint32_t i = 0; i < super->data_blocks; i++)
        if (bit_get(i)) used++;
    info->disk_sectors = super->total_sectors;
    info->start_sector = FS_LBA;
    info->metadata_sectors = DATA_OFF;
    info->data_blocks = super->data_blocks;
    info->used_blocks = used;
    return 0;
}

int fs_empty_recycle_bin(void) {
    int bin = fs_recycle_bin();
    if (bin < 0) return -1;
    for (int pass = 0; pass < INODE_COUNT; pass++) {
        int progress = 0;
        for (int i = 1; i < INODE_COUNT; i++) {
            if (!inodes[i].type || !inside_directory(i, (uint16_t)bin)) continue;
            if (fs_delete(i) == 0) progress = 1;
        }
        int remaining = 0;
        for (int i = 1; i < INODE_COUNT; i++)
            if (inodes[i].type && inside_directory(i, (uint16_t)bin)) {
                remaining = 1;
                break;
            }
        if (!remaining) return fs_sync();
        if (!progress) return -1;
    }
    return -1;
}

int fs_list(uint16_t parent, int *out, int max) {
    int n = 0;
    if (!mounted) return 0;
    for (int i = 1; i < INODE_COUNT && n < max; i++) {
        if (!inodes[i].type) continue;
        if (inodes[i].parent != parent) continue;
        out[n++] = i;
    }
    /* Simple insertion sort by name. */
    for (int i = 1; i < n; i++) {
        int v = out[i];
        int j = i;
        while (j > 0 && strcasecmp(inodes[out[j - 1]].name, inodes[v].name) > 0) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }
    return n;
}

const char *fs_name(int inode) {
    if (inode <= 0 || inode >= INODE_COUNT) return "";
    return inodes[inode].name;
}
uint32_t fs_size(int inode) {
    if (inode <= 0 || inode >= INODE_COUNT) return 0;
    return inodes[inode].size;
}
int fs_type(int inode) {
    if (inode < 0 || inode >= INODE_COUNT) return 0;
    return inodes[inode].type;
}
uint16_t fs_parent(int inode) {
    if (inode <= 0 || inode >= INODE_COUNT) return 0;
    return inodes[inode].parent;
}
int fs_is_dir(int inode) {
    return fs_type(inode) == 2;
}
