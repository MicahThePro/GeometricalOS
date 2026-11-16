#ifndef GOS_RECOVERY_H
#define GOS_RECOVERY_H

#include "bootinfo.h"
#include "ata.h"

int recovery_boot_menu(const BootInfo *bi, const AtaDisk *disk,
                       int disk_result, int fs_result);

#endif
