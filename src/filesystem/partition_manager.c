#include "filesystem/partition_manager.h"

#include "filesystem/blockdev.h"

#define MBR_PARTITION_TABLE_OFFSET 446U
#define MBR_PARTITION_ENTRY_SIZE 16U
#define MBR_SIGNATURE_OFFSET 510U
#define MBR_SIGNATURE_0 0x55U
#define MBR_SIGNATURE_1 0xAAU
#define MBR_TYPE_FAT32 0x0BU
#define MBR_TYPE_FAT32_LBA 0x0CU
#define MBR_TYPE_GPT_PROTECTIVE 0xEEU

static partition_info_t partitions[PARTITION_MANAGER_MAX_PARTITIONS];
static uint32_t partition_count;
static int superfloppy;

static uint16_t read_u16le(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32le(const uint8_t* data) {
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static int is_fat32_type(uint8_t type) {
    return type == MBR_TYPE_FAT32 || type == MBR_TYPE_FAT32_LBA;
}

static int valid_fat32_boot_sector(const uint8_t* sector, uint32_t volume_sectors) {
    uint16_t bytes_per_sector;
    uint16_t reserved_sectors;
    uint32_t total_sectors;

    if (sector[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_0 ||
        sector[MBR_SIGNATURE_OFFSET + 1U] != MBR_SIGNATURE_1) {
        return 0;
    }

    bytes_per_sector = read_u16le(&sector[11]);
    reserved_sectors = read_u16le(&sector[14]);
    total_sectors = read_u32le(&sector[32]);

    return bytes_per_sector == BLOCKDEV_SECTOR_SIZE &&
        sector[13] != 0U && reserved_sectors != 0U &&
        sector[16] != 0U && read_u32le(&sector[36]) != 0U &&
        read_u32le(&sector[44]) >= 2U && total_sectors != 0U &&
        total_sectors <= volume_sectors;
}

static void clear_partitions(void) {
    for (uint32_t index = 0; index < PARTITION_MANAGER_MAX_PARTITIONS; index++) {
        partitions[index].type = 0;
        partitions[index].bootable = 0;
        partitions[index].valid = 0;
        partitions[index].is_selected = 0;
        partitions[index].start_sector = 0;
        partitions[index].sector_count = 0;
    }

    partition_count = 0;
    superfloppy = 0;
}

static int try_select_fat32_partition(uint32_t index) {
    uint8_t boot_sector[BLOCKDEV_SECTOR_SIZE];
    partition_info_t* partition = &partitions[index];
    uint32_t drive_sectors = blockdev_device_sector_count();

    if (!partition->valid || !is_fat32_type(partition->type)) {
        return 0;
    }

    if (blockdev_set_partition(partition->start_sector, partition->sector_count) != 0 ||
        blockdev_read_sector(0, boot_sector) != 0 ||
        !valid_fat32_boot_sector(boot_sector, partition->sector_count)) {
        (void)blockdev_set_partition(0, drive_sectors);
        return 0;
    }

    partition->is_selected = 1;
    return 1;
}

int partition_manager_init(void) {
    uint8_t mbr[BLOCKDEV_SECTOR_SIZE];
    uint32_t drive_sectors;
    int has_partition_entries = 0;
    int has_protective_gpt = 0;

    clear_partitions();
    drive_sectors = blockdev_device_sector_count();

    if (drive_sectors == 0U || blockdev_set_partition(0, drive_sectors) != 0 ||
        blockdev_read_sector(0, mbr) != 0) {
        return -1;
    }

    if (valid_fat32_boot_sector(mbr, drive_sectors)) {
        partitions[0].valid = 1;
        partitions[0].start_sector = 0;
        partitions[0].sector_count = drive_sectors;
        partitions[0].is_selected = 1;
        partition_count = 1;
        superfloppy = 1;
        return 0;
    }

    if (mbr[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_0 ||
        mbr[MBR_SIGNATURE_OFFSET + 1U] != MBR_SIGNATURE_1) {
        return -1;
    }

    for (uint32_t index = 0; index < PARTITION_MANAGER_MAX_PARTITIONS; index++) {
        const uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + index * MBR_PARTITION_ENTRY_SIZE];
        partition_info_t* partition = &partitions[index];

        partition->bootable = (uint8_t)(entry[0] == 0x80U);
        partition->type = entry[4];
        partition->start_sector = read_u32le(&entry[8]);
        partition->sector_count = read_u32le(&entry[12]);

        if (partition->type != 0U || partition->start_sector != 0U || partition->sector_count != 0U) {
            has_partition_entries = 1;
            partition_count = index + 1U;
        }

        if (partition->type == MBR_TYPE_GPT_PROTECTIVE) {
            has_protective_gpt = 1;
        }

        if (partition->type != 0U && partition->sector_count != 0U &&
            partition->start_sector < drive_sectors &&
            partition->sector_count <= drive_sectors - partition->start_sector) {
            partition->valid = 1;
        }
    }

    if (!has_partition_entries || has_protective_gpt) {
        return -1;
    }

    /* Prefer the active FAT32 partition, then fall back to table order. */
    for (uint32_t index = 0; index < partition_count; index++) {
        if (partitions[index].bootable && try_select_fat32_partition(index)) {
            return 0;
        }
    }

    for (uint32_t index = 0; index < partition_count; index++) {
        if (try_select_fat32_partition(index)) {
            return 0;
        }
    }

    (void)blockdev_set_partition(0, drive_sectors);
    return -1;
}

uint32_t partition_manager_count(void) {
    return partition_count;
}

int partition_manager_get(uint32_t index, partition_info_t* out_partition) {
    if (out_partition == 0 || index >= partition_count) {
        return -1;
    }

    *out_partition = partitions[index];
    return 0;
}

int partition_manager_is_superfloppy(void) {
    return superfloppy;
}