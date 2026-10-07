#ifndef PRISMOS_FILESYSTEM_PARTITION_MANAGER_H
#define PRISMOS_FILESYSTEM_PARTITION_MANAGER_H

#include <stdint.h>

#define PARTITION_MANAGER_MAX_PARTITIONS 4U

typedef struct {
    uint8_t type;
    uint8_t bootable;
    uint8_t valid;
    uint8_t is_selected;
    uint32_t start_sector;
    uint32_t sector_count;
} partition_info_t;

/* Scans the primary MBR table and selects the first valid FAT32 volume.
 * Legacy FAT32 volumes formatted directly on the drive are also supported. */
int partition_manager_init(void);
uint32_t partition_manager_count(void);
int partition_manager_get(uint32_t index, partition_info_t* out_partition);
int partition_manager_is_superfloppy(void);

#endif