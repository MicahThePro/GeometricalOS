#ifndef GOS_ATA_H
#define GOS_ATA_H

#include <stdint.h>

typedef struct {
    char model[44];
    uint32_t sectors;
    int present;
} AtaDisk;

/* Talks only to the IDE primary master (I/O 0x1F0). */
int ata_probe(AtaDisk *disk);
int ata_read(uint32_t lba, void *buf, uint32_t count);
int ata_write(uint32_t lba, const void *buf, uint32_t count);
int ata_flush(void);

#endif
