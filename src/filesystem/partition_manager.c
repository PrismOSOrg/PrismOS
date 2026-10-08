#include "filesystem/partition_manager.h"

#include "filesystem/blockdev.h"
#include "filesystem/fat32/fat32.h"

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
    uint8_t fat_count;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t data_start_sector;

    if (sector[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_0 ||
        sector[MBR_SIGNATURE_OFFSET + 1U] != MBR_SIGNATURE_1) {
        return 0;
    }

    bytes_per_sector = read_u16le(&sector[11]);
    reserved_sectors = read_u16le(&sector[14]);
    fat_count = sector[16];
    total_sectors = read_u32le(&sector[32]);
    sectors_per_fat = read_u32le(&sector[36]);

    if (bytes_per_sector != BLOCKDEV_SECTOR_SIZE || sector[13] == 0U
        || reserved_sectors == 0U || fat_count == 0U || sectors_per_fat == 0U
        || read_u32le(&sector[44]) < 2U || total_sectors == 0U
        || total_sectors > volume_sectors
        || reserved_sectors >= volume_sectors
        || sectors_per_fat > (volume_sectors - reserved_sectors) / fat_count) {
        return 0;
    }

    data_start_sector = (uint32_t)reserved_sectors + (uint32_t)fat_count * sectors_per_fat;
    return data_start_sector < volume_sectors;
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

int partition_manager_get_selected_index(uint32_t* out_index) {
    if (out_index == 0 || superfloppy) {
        return -1;
    }

    for (uint32_t index = 0; index < partition_count; index++) {
        if (partitions[index].is_selected) {
            *out_index = index + 1U;
            return 0;
        }
    }

    return -1;
}

int partition_manager_select(uint32_t index) {
    uint8_t boot_sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t previous_index = 0U;
    int had_previous = partition_manager_get_selected_index(&previous_index) == 0;
    partition_info_t* partition;

    if (index == 0U || index > partition_count) {
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }
    if (superfloppy) {
        return PARTITION_MANAGER_ERROR_SUPERFLOPPY;
    }

    partition = &partitions[index - 1U];
    if (!partition->valid) {
        return PARTITION_MANAGER_ERROR_TABLE;
    }
    if (!is_fat32_type(partition->type)) {
        return PARTITION_MANAGER_ERROR_UNSUPPORTED;
    }
    if (blockdev_set_partition(partition->start_sector, partition->sector_count) != 0
        || blockdev_read_sector(0U, boot_sector) != 0
        || !valid_fat32_boot_sector(boot_sector, partition->sector_count)) {
        if (had_previous && previous_index <= partition_count) {
            partition_info_t* previous = &partitions[previous_index - 1U];
            (void)blockdev_set_partition(previous->start_sector, previous->sector_count);
        } else {
            (void)blockdev_set_partition(0U, blockdev_device_sector_count());
        }
        return PARTITION_MANAGER_ERROR_UNSUPPORTED;
    }

    for (uint32_t slot = 0U; slot < partition_count; slot++) {
        partitions[slot].is_selected = (uint8_t)(slot == index - 1U);
    }
    return PARTITION_MANAGER_OK;
}

int partition_manager_get_usage(uint32_t* out_partition_sectors, uint32_t* out_unallocated_sectors) {
    uint32_t allocated = 0U;
    uint32_t drive_sectors = blockdev_device_sector_count();

    if (out_partition_sectors == 0 || out_unallocated_sectors == 0 || drive_sectors == 0U || superfloppy) {
        return -1;
    }

    for (uint32_t index = 0; index < partition_count; index++) {
        if (partitions[index].valid) {
            if (allocated > drive_sectors - partitions[index].sector_count) {
                return -1;
            }
            allocated += partitions[index].sector_count;
        }
    }

    *out_partition_sectors = allocated;
    *out_unallocated_sectors = drive_sectors - allocated;
    return 0;
}

static int read_full_drive_mbr(uint8_t* mbr, uint32_t* out_drive_sectors) {
    uint32_t drive_sectors = blockdev_device_sector_count();
    uint32_t restore_start = 0U;
    uint32_t restore_count = drive_sectors;

    if (mbr == 0 || out_drive_sectors == 0 || drive_sectors == 0U) {
        return -1;
    }

    if (!superfloppy) {
        int found_selected = 0;
        for (uint32_t index = 0; index < partition_count; index++) {
            if (partitions[index].is_selected) {
                restore_start = partitions[index].start_sector;
                restore_count = partitions[index].sector_count;
                found_selected = 1;
                break;
            }
        }
        if (!found_selected) {
            return -1;
        }
    }

    if (blockdev_set_partition(0U, drive_sectors) != 0 || blockdev_read_sector(0U, mbr) != 0) {
        (void)blockdev_set_partition(restore_start, restore_count);
        return -1;
    }

    *out_drive_sectors = drive_sectors;
    return 0;
}

static int write_full_drive_mbr(const uint8_t* mbr, uint32_t drive_sectors) {
    uint32_t restore_start = 0U;
    uint32_t restore_count = drive_sectors;

    if (mbr == 0) {
        return -1;
    }

    if (!superfloppy) {
        int found_selected = 0;
        for (uint32_t index = 0; index < partition_count; index++) {
            if (partitions[index].is_selected) {
                restore_start = partitions[index].start_sector;
                restore_count = partitions[index].sector_count;
                found_selected = 1;
                break;
            }
        }
        if (!found_selected) {
            return -1;
        }
    }

    if (blockdev_write_sector(0U, mbr) != 0) {
        (void)blockdev_set_partition(restore_start, restore_count);
        return -1;
    }

    return blockdev_set_partition(restore_start, restore_count);
}

static int mbr_entry_is_empty(const uint8_t* entry) {
    return entry[4] == 0U && read_u32le(&entry[8]) == 0U && read_u32le(&entry[12]) == 0U;
}

static int validate_mbr_entries(const uint8_t* mbr, uint32_t drive_sectors) {
    uint32_t starts[PARTITION_MANAGER_MAX_PARTITIONS];
    uint32_t counts[PARTITION_MANAGER_MAX_PARTITIONS];

    for (uint32_t index = 0; index < PARTITION_MANAGER_MAX_PARTITIONS; index++) {
        const uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + index * MBR_PARTITION_ENTRY_SIZE];
        uint32_t start = read_u32le(&entry[8]);
        uint32_t count = read_u32le(&entry[12]);

        starts[index] = start;
        counts[index] = count;
        if (entry[4] == 0U) {
            if (start != 0U || count != 0U) {
                return -1;
            }
            continue;
        }

        if (start == 0U || count == 0U || start >= drive_sectors || count > drive_sectors - start) {
            return -1;
        }
    }

    for (uint32_t left = 0; left < PARTITION_MANAGER_MAX_PARTITIONS; left++) {
        if (counts[left] == 0U) {
            continue;
        }
        for (uint32_t right = left + 1U; right < PARTITION_MANAGER_MAX_PARTITIONS; right++) {
            if (counts[right] == 0U) {
                continue;
            }
            if (starts[left] < starts[right] + counts[right]
                && starts[right] < starts[left] + counts[left]) {
                return -1;
            }
        }
    }

    return 0;
}

int partition_manager_create(uint32_t size_mib, uint32_t* out_index, uint32_t* out_start_sector) {
    uint8_t mbr[BLOCKDEV_SECTOR_SIZE];
    uint32_t drive_sectors;
    uint32_t requested_sectors;
    uint32_t candidate = 2048U;
    uint32_t empty_slot = PARTITION_MANAGER_MAX_PARTITIONS;
    int result = PARTITION_MANAGER_ERROR_TABLE;

    if (size_mib == 0U || out_index == 0 || out_start_sector == 0) {
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }
    if (superfloppy) {
        return PARTITION_MANAGER_ERROR_SUPERFLOPPY;
    }
    if (read_full_drive_mbr(mbr, &drive_sectors) != 0) {
        return PARTITION_MANAGER_ERROR_IO;
    }

    if (mbr[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_0 || mbr[MBR_SIGNATURE_OFFSET + 1U] != MBR_SIGNATURE_1
        || validate_mbr_entries(mbr, drive_sectors) != 0) {
        goto done;
    }

    if (size_mib > 0xFFFFFFFFU / 2048U) {
        result = PARTITION_MANAGER_ERROR_ARGUMENT;
        goto done;
    }
    requested_sectors = size_mib * 2048U;

    for (uint32_t index = 0; index < PARTITION_MANAGER_MAX_PARTITIONS; index++) {
        const uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + index * MBR_PARTITION_ENTRY_SIZE];
        if (mbr_entry_is_empty(entry)) {
            empty_slot = index;
            break;
        }
    }
    if (empty_slot >= PARTITION_MANAGER_MAX_PARTITIONS) {
        result = PARTITION_MANAGER_ERROR_NO_SLOT;
        goto done;
    }
    if (candidate >= drive_sectors || requested_sectors > drive_sectors - candidate) {
        result = PARTITION_MANAGER_ERROR_NO_SPACE;
        goto done;
    }

    for (;;) {
        uint32_t candidate_end = candidate + requested_sectors;
        uint32_t next_candidate = candidate;

        if (candidate_end > drive_sectors) {
            result = PARTITION_MANAGER_ERROR_NO_SPACE;
            goto done;
        }

        for (uint32_t index = 0; index < PARTITION_MANAGER_MAX_PARTITIONS; index++) {
            const uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + index * MBR_PARTITION_ENTRY_SIZE];
            uint32_t existing_start = read_u32le(&entry[8]);
            uint32_t existing_count = read_u32le(&entry[12]);
            uint32_t existing_end = existing_start + existing_count;

            if (existing_count != 0U && candidate < existing_end && existing_start < candidate_end
                && existing_end > next_candidate) {
                next_candidate = existing_end;
            }
        }

        if (next_candidate == candidate) {
            break;
        }
        if (next_candidate > 0xFFFFFFFFU - 2047U) {
            result = PARTITION_MANAGER_ERROR_NO_SPACE;
            goto done;
        }
        candidate = ((next_candidate + 2047U) / 2048U) * 2048U;
    }

    {
        uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + empty_slot * MBR_PARTITION_ENTRY_SIZE];
        for (uint32_t index = 0; index < MBR_PARTITION_ENTRY_SIZE; index++) {
            entry[index] = 0U;
        }
        entry[4] = MBR_TYPE_FAT32_LBA;
        entry[8] = (uint8_t)(candidate & 0xFFU);
        entry[9] = (uint8_t)((candidate >> 8) & 0xFFU);
        entry[10] = (uint8_t)((candidate >> 16) & 0xFFU);
        entry[11] = (uint8_t)((candidate >> 24) & 0xFFU);
        entry[12] = (uint8_t)(requested_sectors & 0xFFU);
        entry[13] = (uint8_t)((requested_sectors >> 8) & 0xFFU);
        entry[14] = (uint8_t)((requested_sectors >> 16) & 0xFFU);
        entry[15] = (uint8_t)((requested_sectors >> 24) & 0xFFU);
    }

    if (write_full_drive_mbr(mbr, drive_sectors) == 0) {
        *out_index = empty_slot + 1U;
        *out_start_sector = candidate;
        result = PARTITION_MANAGER_OK;
    } else {
        result = PARTITION_MANAGER_ERROR_IO;
    }

done:
    if (result != 0) {
        uint32_t restore_drive_sectors = blockdev_device_sector_count();
        uint32_t restore_start = 0U;
        uint32_t restore_count = restore_drive_sectors;
        if (!superfloppy) {
            for (uint32_t index = 0; index < partition_count; index++) {
                if (partitions[index].is_selected) {
                    restore_start = partitions[index].start_sector;
                    restore_count = partitions[index].sector_count;
                    break;
                }
            }
        }
        (void)blockdev_set_partition(restore_start, restore_count);
    }
    return result;
}

int partition_manager_delete(uint32_t index) {
    uint8_t mbr[BLOCKDEV_SECTOR_SIZE];
    partition_info_t partition;
    uint32_t drive_sectors;
    uint32_t slot;
    int result = PARTITION_MANAGER_ERROR_TABLE;

    if (index == 0U || index > PARTITION_MANAGER_MAX_PARTITIONS) {
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }
    if (superfloppy) {
        return PARTITION_MANAGER_ERROR_SUPERFLOPPY;
    }
    if (partition_manager_get(index - 1U, &partition) != 0) {
        return PARTITION_MANAGER_ERROR_NO_SLOT;
    }
    if (!partition.valid) {
        return PARTITION_MANAGER_ERROR_TABLE;
    }
    if (partition.is_selected) {
        return PARTITION_MANAGER_ERROR_PROTECTED;
    }
    if (partition.type == 0x05U || partition.type == 0x0FU
        || partition.type == 0x85U || partition.type == MBR_TYPE_GPT_PROTECTIVE) {
        return PARTITION_MANAGER_ERROR_UNSUPPORTED;
    }
    if (read_full_drive_mbr(mbr, &drive_sectors) != 0) {
        return PARTITION_MANAGER_ERROR_IO;
    }

    (void)drive_sectors;
    slot = index - 1U;
    {
        uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + slot * MBR_PARTITION_ENTRY_SIZE];
        if (read_u32le(&entry[8]) != partition.start_sector
            || read_u32le(&entry[12]) != partition.sector_count
            || entry[4] != partition.type) {
            goto done;
        }
        for (uint32_t offset = 0; offset < MBR_PARTITION_ENTRY_SIZE; offset++) {
            entry[offset] = 0U;
        }
    }

    if (write_full_drive_mbr(mbr, blockdev_device_sector_count()) == 0) {
        result = PARTITION_MANAGER_OK;
    } else {
        result = PARTITION_MANAGER_ERROR_IO;
    }

done:
    if (result != 0 && !partition_manager_is_superfloppy()) {
        uint32_t selected_index;
        if (partition_manager_get_selected_index(&selected_index) == 0) {
            partition_info_t selected;
            if (partition_manager_get(selected_index - 1U, &selected) == 0) {
                (void)blockdev_set_partition(selected.start_sector, selected.sector_count);
            }
        }
    }
    return result;
}

int partition_manager_shrink(uint32_t index, uint32_t shrink_mib) {
    uint8_t mbr[BLOCKDEV_SECTOR_SIZE];
    partition_info_t partition;
    uint32_t drive_sectors;
    uint32_t shrink_sectors;
    uint32_t new_sector_count;
    uint32_t slot;
    int result;

    if (index == 0U || index > PARTITION_MANAGER_MAX_PARTITIONS || shrink_mib == 0U
        || shrink_mib > 0xFFFFFFFFU / 2048U) {
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }
    if (superfloppy) {
        return PARTITION_MANAGER_ERROR_SUPERFLOPPY;
    }
    if (partition_manager_get(index - 1U, &partition) != 0) {
        return PARTITION_MANAGER_ERROR_NO_SLOT;
    }
    if (!partition.valid) {
        return PARTITION_MANAGER_ERROR_TABLE;
    }
    if (!partition.is_selected || (partition.type != MBR_TYPE_FAT32 && partition.type != MBR_TYPE_FAT32_LBA)) {
        return PARTITION_MANAGER_ERROR_PROTECTED;
    }

    shrink_sectors = shrink_mib * 2048U;
    if (shrink_sectors >= partition.sector_count) {
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }
    new_sector_count = partition.sector_count - shrink_sectors;
    slot = index - 1U;

    if (read_full_drive_mbr(mbr, &drive_sectors) != 0) {
        return PARTITION_MANAGER_ERROR_IO;
    }
    if (mbr[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_0
        || mbr[MBR_SIGNATURE_OFFSET + 1U] != MBR_SIGNATURE_1
        || validate_mbr_entries(mbr, drive_sectors) != 0) {
        (void)blockdev_set_partition(partition.start_sector, partition.sector_count);
        return PARTITION_MANAGER_ERROR_TABLE;
    }

    {
        uint8_t* entry = &mbr[MBR_PARTITION_TABLE_OFFSET + slot * MBR_PARTITION_ENTRY_SIZE];
        if (read_u32le(&entry[8]) != partition.start_sector
            || read_u32le(&entry[12]) != partition.sector_count
            || entry[4] != partition.type) {
            (void)blockdev_set_partition(partition.start_sector, partition.sector_count);
            return PARTITION_MANAGER_ERROR_TABLE;
        }
        entry[12] = (uint8_t)(new_sector_count & 0xFFU);
        entry[13] = (uint8_t)((new_sector_count >> 8) & 0xFFU);
        entry[14] = (uint8_t)((new_sector_count >> 16) & 0xFFU);
        entry[15] = (uint8_t)((new_sector_count >> 24) & 0xFFU);
    }

    /* read_full_drive_mbr() leaves whole-drive access enabled. Restore the old
     * mounted window before touching FAT32's partition-relative boot sectors. */
    if (blockdev_set_partition(partition.start_sector, partition.sector_count) != 0) {
        return PARTITION_MANAGER_ERROR_IO;
    }
    result = fat32_shrink_volume(new_sector_count);
    if (result != FAT32_RESIZE_OK) {
        if (result == FAT32_RESIZE_ERROR_IO) {
            return PARTITION_MANAGER_ERROR_IO;
        }
        if (result == FAT32_RESIZE_ERROR_DATA_PRESENT) {
            return PARTITION_MANAGER_ERROR_DATA_PRESENT;
        }
        return PARTITION_MANAGER_ERROR_ARGUMENT;
    }

    /* The filesystem is now valid within the smaller boundary. If this write
     * fails, the old MBR still safely contains the smaller filesystem, and
     * repeating the same shrink will finish the table update. */
    if (blockdev_set_partition(0U, drive_sectors) != 0
        || write_full_drive_mbr(mbr, drive_sectors) != 0) {
        return PARTITION_MANAGER_ERROR_PARTIAL;
    }

    partitions[slot].sector_count = new_sector_count;
    if (blockdev_set_partition(partition.start_sector, new_sector_count) != 0) {
        return PARTITION_MANAGER_ERROR_PARTIAL;
    }
    return PARTITION_MANAGER_OK;
}