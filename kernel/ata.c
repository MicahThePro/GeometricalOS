#include "ata.h"
#include "io.h"
#include "serial.h"

#define ATA_IO 0x1F0
#define ATA_CTRL 0x3F6
#define RAMDISK_SECTORS 16384u

static uint8_t *ramdisk;
static int ramdisk_on;

int ata_ramdisk_begin(void) {
    ramdisk = (uint8_t *)0x2800000;
    memset(ramdisk, 0, RAMDISK_SECTORS * 512u);
    ramdisk_on = 1;
    serial_write("ATA: memory disk\n");
    return 0;
}

static int wait_not_busy(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_IO + 7);
        if (!(s & 0x80)) return 0;
    }
    return -1;
}

static int wait_drq(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_IO + 7);
        if (s & 0x80) continue;
        if (s & 0x01) return -1;
        if (s & 0x08) return 0;
        if (s & 0x21) return -1; /* ERR or DF */
    }
    return -2;
}

static void select_master_lba(uint32_t lba) {
    outb(ATA_IO + 6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

static void trim_model(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) s[--n] = 0;
    int i = 0;
    while (s[i] == ' ') i++;
    if (i) {
        int j = 0;
        while (s[i]) s[j++] = s[i++];
        s[j] = 0;
    }
}

int ata_probe(AtaDisk *disk) {
    memset(disk, 0, sizeof(*disk));
    if (ramdisk_on) {
        memcpy(disk->model, "Memory disk", 12);
        disk->sectors = RAMDISK_SECTORS;
        disk->present = 1;
        return 0;
    }
    uint8_t st = inb(ATA_IO + 7);
    if (st == 0xFF) {
        serial_write("ATA: no primary controller\n");
        return -1;
    }
    outb(ATA_CTRL, 0x02); /* disable device IRQs */
    outb(ATA_IO + 6, 0xA0);
    inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL);
    outb(ATA_IO + 2, 0);
    outb(ATA_IO + 3, 0);
    outb(ATA_IO + 4, 0);
    outb(ATA_IO + 5, 0);
    outb(ATA_IO + 7, 0xEC);
    st = inb(ATA_IO + 7);
    if (st == 0) {
        serial_write("ATA: no primary master\n");
        return -1;
    }
    if (wait_drq() != 0) {
        uint8_t mid = inb(ATA_IO + 4);
        uint8_t hi = inb(ATA_IO + 5);
        if (mid == 0x14 && hi == 0xEB) {
            serial_write("ATA: primary master is ATAPI\n");
            return -2;
        }
        serial_write("ATA: identify failed\n");
        return -3;
    }
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_IO);
    for (int i = 0; i < 20; i++) {
        disk->model[i * 2] = (char)(id[27 + i] >> 8);
        disk->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    disk->model[40] = 0;
    trim_model(disk->model);
    uint32_t sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    uint32_t s48 = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
    if (s48 > sectors) sectors = s48;
    disk->sectors = sectors;
    disk->present = sectors > 0;
    serial_write("ATA: ");
    serial_write(disk->model[0] ? disk->model : "(unnamed disk)");
    serial_write(" sectors ");
    serial_dec(sectors);
    serial_write("\n");
    return disk->present ? 0 : -4;
}

int ata_read(uint32_t lba, void *buf, uint32_t count) {
    if (ramdisk_on) {
        if (lba + count < lba || lba + count > RAMDISK_SECTORS) return -1;
        memcpy(buf, ramdisk + lba * 512u, count * 512u);
        return 0;
    }
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count > 16 ? 16 : count;
        if (wait_not_busy()) return -1;
        select_master_lba(lba);
        if (wait_not_busy()) return -1;
        outb(ATA_IO + 2, (uint8_t)n);
        outb(ATA_IO + 3, (uint8_t)lba);
        outb(ATA_IO + 4, (uint8_t)(lba >> 8));
        outb(ATA_IO + 5, (uint8_t)(lba >> 16));
        outb(ATA_IO + 7, 0x20);
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq()) return -2;
            uint16_t *p = (uint16_t *)(out + (s * 512));
            for (int i = 0; i < 256; i++) p[i] = inw(ATA_IO);
        }
        lba += n;
        count -= n;
        out += n * 512;
    }
    return 0;
}

int ata_write(uint32_t lba, const void *buf, uint32_t count) {
    if (ramdisk_on) {
        if (lba + count < lba || lba + count > RAMDISK_SECTORS) return -1;
        memcpy((void *)(ramdisk + lba * 512u), buf, count * 512u);
        return 0;
    }
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count > 16 ? 16 : count;
        if (wait_not_busy()) return -1;
        select_master_lba(lba);
        if (wait_not_busy()) return -1;
        outb(ATA_IO + 2, (uint8_t)n);
        outb(ATA_IO + 3, (uint8_t)lba);
        outb(ATA_IO + 4, (uint8_t)(lba >> 8));
        outb(ATA_IO + 5, (uint8_t)(lba >> 16));
        outb(ATA_IO + 7, 0x30);
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq()) return -2;
            const uint16_t *p = (const uint16_t *)(in + (s * 512));
            for (int i = 0; i < 256; i++) outw(ATA_IO, p[i]);
        }
        lba += n;
        count -= n;
        in += n * 512;
    }
    if (wait_not_busy()) return -3;
    if (ata_flush()) return -4;
    return 0;
}

int ata_flush(void) {
    if (ramdisk_on) return 0;
    if (wait_not_busy()) return -1;
    outb(ATA_IO + 6, 0xE0);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    outb(ATA_IO + 7, 0xE7); /* FLUSH CACHE */
    if (wait_not_busy()) return -1;
    uint8_t st = inb(ATA_IO + 7);
    if (st & 0x01) {
        outb(ATA_IO + 7, 0xEA); /* FLUSH CACHE EXT, if the drive understands it */
        wait_not_busy();
    }
    return 0;
}
