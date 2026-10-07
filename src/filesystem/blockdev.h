#ifndef PRISMOS_FILESYSTEM_BLOCKDEV_H
#define PRISMOS_FILESYSTEM_BLOCKDEV_H

#include <stdint.h>

#define BLOCKDEV_SECTOR_SIZE 512U
#define BLOCKDEV_MAX_DRIVES 4U

typedef struct {
	uint32_t sector_count;
} blockdev_drive_info_t;

/* The sector interface exposes the currently selected volume, not the whole drive. */
int blockdev_init_disk(void);
uint32_t blockdev_drive_count(void);
uint32_t blockdev_current_drive(void);
int blockdev_get_drive_info(uint32_t drive, blockdev_drive_info_t* out_info);
int blockdev_select_drive(uint32_t drive);
int blockdev_read_sector(uint32_t sector, void* buffer);
int blockdev_write_sector(uint32_t sector, const void* buffer);
uint32_t blockdev_sector_count(void);
uint32_t blockdev_device_sector_count(void);
int blockdev_set_partition(uint32_t start_sector, uint32_t sector_count);

#endif
