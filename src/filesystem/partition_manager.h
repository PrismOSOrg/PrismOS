#ifndef PRISMOS_FILESYSTEM_PARTITION_MANAGER_H
#define PRISMOS_FILESYSTEM_PARTITION_MANAGER_H

#include <stdint.h>

#define PARTITION_MANAGER_MAX_PARTITIONS 4U

enum {
    PARTITION_MANAGER_OK = 0,
    PARTITION_MANAGER_ERROR_ARGUMENT = -1,
    PARTITION_MANAGER_ERROR_SUPERFLOPPY = -2,
    PARTITION_MANAGER_ERROR_TABLE = -3,
    PARTITION_MANAGER_ERROR_NO_SLOT = -4,
    PARTITION_MANAGER_ERROR_NO_SPACE = -5,
    PARTITION_MANAGER_ERROR_PROTECTED = -6,
    PARTITION_MANAGER_ERROR_IO = -7,
    PARTITION_MANAGER_ERROR_UNSUPPORTED = -8,
    PARTITION_MANAGER_ERROR_DATA_PRESENT = -9,
    PARTITION_MANAGER_ERROR_PARTIAL = -10,
};

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
int partition_manager_get_selected_index(uint32_t* out_index);
int partition_manager_select(uint32_t index);
int partition_manager_get_usage(uint32_t* out_partition_sectors, uint32_t* out_unallocated_sectors);
int partition_manager_create(uint32_t size_mib, uint32_t* out_index, uint32_t* out_start_sector);
int partition_manager_delete(uint32_t index);
int partition_manager_shrink(uint32_t index, uint32_t shrink_mib);

#endif