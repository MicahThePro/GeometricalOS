#ifndef GOS_FS_H
#define GOS_FS_H

#include <stdint.h>

#define FS_ROOT 0

int fs_format(uint32_t disk_sectors);
int fs_mount(void);
int fs_sync(void);

int fs_lookup(uint16_t parent, const char *name);
int fs_create_dir(uint16_t parent, const char *name);
int fs_write(uint16_t parent, const char *name, const void *data, uint32_t size);
int fs_read(int inode, void *buf, uint32_t max, uint32_t *out_size);
int fs_delete(int inode);
int fs_recycle(int inode);
int fs_restore(int inode);
int fs_empty_recycle_bin(void);
int fs_recycle_bin(void);
int fs_list(uint16_t parent, int *out, int max);

const char *fs_name(int inode);
uint32_t fs_size(int inode);
int fs_type(int inode);          /* 1 file, 2 directory */
uint16_t fs_parent(int inode);
int fs_is_dir(int inode);

#endif
