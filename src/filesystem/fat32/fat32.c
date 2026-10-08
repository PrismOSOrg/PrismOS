#include "filesystem/fat32/fat32.h"

#include <stddef.h>

#include "debug/log.h"
#include "filesystem/blockdev.h"

#define FAT32_ATTR_DIRECTORY 0x10U
#define FAT32_ATTR_VOLUME_ID 0x08U
#define FAT32_ATTR_LFN 0x0FU
#define FAT32_CLUSTER_FREE 0x00000000U
#define FAT32_CLUSTER_EOC 0x0FFFFFFFU
#define FAT32_CLUSTER_EOC_MIN 0x0FFFFFF8U
#define FAT32_MIN_CLUSTER_COUNT 65525U

typedef struct {
    int mounted;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t fat_start_sector;
    uint32_t data_start_sector;
} Fat32State;

typedef struct {
    uint32_t sector;
    uint32_t offset;
    uint8_t entry[32];
} DirEntryRef;

static Fat32State fs = {0};

static int allocate_cluster(uint32_t* out_cluster);
static int free_cluster_chain(uint32_t start_cluster);

static uint16_t read_u16le(const uint8_t* source) {
    return (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
}

static uint32_t read_u32le(const uint8_t* source) {
    return (uint32_t)source[0]
        | ((uint32_t)source[1] << 8)
        | ((uint32_t)source[2] << 16)
        | ((uint32_t)source[3] << 24);
}

static void write_u16le(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_u32le(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void memory_set(uint8_t* destination, uint8_t value, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        destination[index] = value;
    }
}

static void memory_copy(uint8_t* destination, const uint8_t* source, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        destination[index] = source[index];
    }
}

static int memory_equal(const uint8_t* left, const uint8_t* right, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }

    return 1;
}

static uint8_t to_upper(uint8_t c) {
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(c - ('a' - 'A'));
    }

    return c;
}

static uint32_t entry_cluster(const uint8_t* entry) {
    uint32_t high = (uint32_t)read_u16le(&entry[20]);
    uint32_t low = (uint32_t)read_u16le(&entry[26]);
    return (high << 16) | low;
}

static void set_entry_cluster(uint8_t* entry, uint32_t cluster) {
    write_u16le(&entry[20], (uint16_t)((cluster >> 16) & 0xFFFFU));
    write_u16le(&entry[26], (uint16_t)(cluster & 0xFFFFU));
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return fs.data_start_sector + ((cluster - 2U) * (uint32_t)fs.sectors_per_cluster);
}

static uint32_t cluster_bytes(void) {
    return (uint32_t)fs.bytes_per_sector * (uint32_t)fs.sectors_per_cluster;
}

static uint32_t fat_entry_capacity(void) {
    return ((uint32_t)fs.sectors_per_fat * (uint32_t)BLOCKDEV_SECTOR_SIZE) / 4U;
}

static int is_data_cluster(uint32_t cluster) {
    return cluster >= 2U && cluster < FAT32_CLUSTER_EOC_MIN;
}

static uint32_t clamp_cluster_limit(uint32_t by_layout) {
    uint32_t by_fat = fat_entry_capacity();

    if (by_fat < 2U) {
        return 2U;
    }

    if (by_layout < by_fat) {
        return by_layout;
    }

    return by_fat;
}

static uint32_t read_fat_entry(uint32_t cluster) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t fat_offset = cluster * 4U;
    uint32_t sector_offset = fat_offset / BLOCKDEV_SECTOR_SIZE;
    uint32_t entry_offset = fat_offset % BLOCKDEV_SECTOR_SIZE;

    if (cluster >= fat_entry_capacity()) {
        return FAT32_CLUSTER_EOC;
    }

    if (blockdev_read_sector(fs.fat_start_sector + sector_offset, sector) != 0) {
        ERROR_LOG("failed to read FAT sector");
        return FAT32_CLUSTER_EOC;
    }

    return read_u32le(&sector[entry_offset]) & FAT32_CLUSTER_EOC;
}

static int write_fat_entry(uint32_t cluster, uint32_t value) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t fat_offset = cluster * 4U;
    uint32_t sector_offset = fat_offset / BLOCKDEV_SECTOR_SIZE;
    uint32_t entry_offset = fat_offset % BLOCKDEV_SECTOR_SIZE;

    if (cluster >= fat_entry_capacity()) {
        return -1;
    }

    if (blockdev_read_sector(fs.fat_start_sector + sector_offset, sector) != 0) {
        return -1;
    }

    write_u32le(&sector[entry_offset], value & FAT32_CLUSTER_EOC);

    if (blockdev_write_sector(fs.fat_start_sector + sector_offset, sector) != 0) {
        return -1;
    }

    return 0;
}

static int make_display_name(const uint8_t* raw, char* out_name) {
    int out_index = 0;
    int saw_name_char = 0;

    for (int index = 0; index < 8; index++) {
        uint8_t c = raw[index];
        if (c == ' ') {
            break;
        }

        out_name[out_index++] = (char)c;
        saw_name_char = 1;
    }

    if (!saw_name_char) {
        return -1;
    }

    if (raw[8] != ' ') {
        out_name[out_index++] = '.';

        for (int index = 8; index < 11; index++) {
            uint8_t c = raw[index];
            if (c == ' ') {
                break;
            }

            out_name[out_index++] = (char)c;
        }
    }

    out_name[out_index] = '\0';
    return 0;
}

static int component_to_short_name(const char* component, uint8_t out[11]) {
    int base_length = 0;
    int extension_length = 0;
    int index = 0;

    while (index < 11) {
        out[index] = ' ';
        index++;
    }

    index = 0;
    while (component[index] != '\0' && component[index] != '.') {
        if (base_length >= 8) {
            return -1;
        }

        if (component[index] == '/' || component[index] == ' ') {
            return -1;
        }

        out[base_length] = to_upper((uint8_t)component[index]);
        base_length++;
        index++;
    }

    if (base_length == 0) {
        return -1;
    }

    if (component[index] == '.') {
        index++;
        while (component[index] != '\0') {
            if (extension_length >= 3) {
                return -1;
            }

            if (component[index] == '/' || component[index] == ' ' || component[index] == '.') {
                return -1;
            }

            out[8 + extension_length] = to_upper((uint8_t)component[index]);
            extension_length++;
            index++;
        }
    }

    return 0;
}

static int next_path_component(const char** cursor, char* out_component, int out_capacity) {
    int length = 0;
    const char* path = *cursor;

    while (*path == '/') {
        path++;
    }

    if (*path == '\0') {
        *cursor = path;
        return 0;
    }

    while (*path != '\0' && *path != '/') {
        if (length >= (out_capacity - 1)) {
            return -1;
        }

        out_component[length++] = *path;
        path++;
    }

    out_component[length] = '\0';
    *cursor = path;
    return 1;
}

static uint32_t max_cluster_index(void) {
    uint32_t data_sectors = fs.total_sectors - fs.data_start_sector;
    uint32_t clusters = data_sectors / (uint32_t)fs.sectors_per_cluster;
    return clamp_cluster_limit(2U + clusters);
}

static int read_cluster_bytes(uint32_t cluster, uint32_t offset, uint8_t* out, uint32_t size) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t bytes = cluster_bytes();
    uint32_t local_offset = offset;
    uint32_t copied = 0;

    if (!is_data_cluster(cluster) || out == NULL || size == 0U || local_offset + size > bytes) {
        return -1;
    }

    while (copied < size) {
        uint32_t sector_index = local_offset / BLOCKDEV_SECTOR_SIZE;
        uint32_t sector_offset = local_offset % BLOCKDEV_SECTOR_SIZE;
        uint32_t chunk = BLOCKDEV_SECTOR_SIZE - sector_offset;
        if (chunk > (size - copied)) {
            chunk = size - copied;
        }

        if (blockdev_read_sector(cluster_to_sector(cluster) + sector_index, sector) != 0) {
            return -1;
        }

        memory_copy(&out[copied], &sector[sector_offset], chunk);
        copied += chunk;
        local_offset += chunk;
    }

    return 0;
}

static int write_cluster_bytes(uint32_t cluster, uint32_t offset, const uint8_t* data, uint32_t size) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t bytes = cluster_bytes();
    uint32_t local_offset = offset;
    uint32_t copied = 0;

    if (!is_data_cluster(cluster) || data == NULL || size == 0U || local_offset + size > bytes) {
        return -1;
    }

    while (copied < size) {
        uint32_t sector_index = local_offset / BLOCKDEV_SECTOR_SIZE;
        uint32_t sector_offset = local_offset % BLOCKDEV_SECTOR_SIZE;
        uint32_t chunk = BLOCKDEV_SECTOR_SIZE - sector_offset;
        if (chunk > (size - copied)) {
            chunk = size - copied;
        }

        if (blockdev_read_sector(cluster_to_sector(cluster) + sector_index, sector) != 0) {
            return -1;
        }

        memory_copy(&sector[sector_offset], &data[copied], chunk);

        if (blockdev_write_sector(cluster_to_sector(cluster) + sector_index, sector) != 0) {
            return -1;
        }

        copied += chunk;
        local_offset += chunk;
    }

    return 0;
}

static int ensure_file_cluster_chain(uint32_t* inout_first_cluster, uint32_t required_clusters) {
    uint32_t first = *inout_first_cluster;
    uint32_t current;
    uint32_t count;
    uint32_t next;

    if (required_clusters == 0U) {
        return 0;
    }

    if (!is_data_cluster(first)) {
        if (allocate_cluster(&first) != 0) {
            return -1;
        }
    }

    current = first;
    count = 1U;

    while (count < required_clusters) {
        next = read_fat_entry(current);
        if (!is_data_cluster(next)) {
            uint32_t allocated;
            if (allocate_cluster(&allocated) != 0) {
                return -1;
            }

            if (write_fat_entry(current, allocated) != 0) {
                return -1;
            }

            current = allocated;
        } else {
            current = next;
        }

        count++;
    }

    next = read_fat_entry(current);
    if (write_fat_entry(current, FAT32_CLUSTER_EOC) != 0) {
        return -1;
    }

    if (is_data_cluster(next) && free_cluster_chain(next) != 0) {
        return -1;
    }

    *inout_first_cluster = first;
    return 0;
}

static int clear_cluster(uint32_t cluster) {
    uint8_t zero_sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t sector = cluster_to_sector(cluster);

    memory_set(zero_sector, 0, sizeof(zero_sector));

    for (uint32_t s = 0; s < (uint32_t)fs.sectors_per_cluster; s++) {
        if (blockdev_write_sector(sector + s, zero_sector) != 0) {
            return -1;
        }
    }

    return 0;
}

static int allocate_cluster(uint32_t* out_cluster) {
    uint32_t limit = max_cluster_index();

    for (uint32_t cluster = 2U; cluster < limit; cluster++) {
        if (read_fat_entry(cluster) == FAT32_CLUSTER_FREE) {
            if (write_fat_entry(cluster, FAT32_CLUSTER_EOC) != 0) {
                return -1;
            }

            if (clear_cluster(cluster) != 0) {
                return -1;
            }

            *out_cluster = cluster;
            return 0;
        }
    }

    return -1;
}

static int free_cluster_chain(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t guard = 0;

    while (is_data_cluster(cluster)) {
        uint32_t next = read_fat_entry(cluster);
        if (write_fat_entry(cluster, FAT32_CLUSTER_FREE) != 0) {
            return -1;
        }

        cluster = next;
        guard++;
        if (guard > 4096U) {
            return -1;
        }
    }

    return 0;
}

static int find_entry_in_directory(
    uint32_t directory_cluster,
    const uint8_t short_name[11],
    DirEntryRef* out_match,
    uint32_t* out_free_sector,
    uint32_t* out_free_offset
) {
    uint32_t cluster = directory_cluster;
    uint32_t guard = 0;
    int has_free_slot = 0;

    while (cluster >= 2U && cluster < 0x0FFFFFF8U) {
        uint32_t start_sector = cluster_to_sector(cluster);

        for (uint32_t s = 0; s < (uint32_t)fs.sectors_per_cluster; s++) {
            uint8_t sector[BLOCKDEV_SECTOR_SIZE];

            if (blockdev_read_sector(start_sector + s, sector) != 0) {
                return -1;
            }

            for (uint32_t offset = 0; offset < BLOCKDEV_SECTOR_SIZE; offset += 32U) {
                uint8_t first = sector[offset];
                uint8_t attr = sector[offset + 11U];

                if (first == 0x00U) {
                    if (!has_free_slot) {
                        *out_free_sector = start_sector + s;
                        *out_free_offset = offset;
                    }
                    return 0;
                }

                if (first == 0xE5U || attr == FAT32_ATTR_LFN) {
                    if (!has_free_slot) {
                        has_free_slot = 1;
                        *out_free_sector = start_sector + s;
                        *out_free_offset = offset;
                    }
                    continue;
                }

                if (memory_equal(&sector[offset], short_name, 11U)) {
                    out_match->sector = start_sector + s;
                    out_match->offset = offset;
                    memory_copy(out_match->entry, &sector[offset], 32U);
                    return 1;
                }
            }
        }

        cluster = read_fat_entry(cluster);
        guard++;
        if (guard > 4096U) {
            return -1;
        }
    }

    return -1;
}

static int resolve_directory_cluster(const char* abs_path, uint32_t* out_cluster) {
    const char* cursor = abs_path;
    uint32_t current = fs.root_cluster;
    char component[13];

    if (abs_path == NULL || abs_path[0] != '/') {
        return -1;
    }

    if (abs_path[1] == '\0') {
        *out_cluster = fs.root_cluster;
        return 0;
    }

    while (1) {
        DirEntryRef match;
        uint32_t free_sector = 0;
        uint32_t free_offset = 0;
        uint8_t short_name[11];
        int status = next_path_component(&cursor, component, (int)sizeof(component));

        if (status == 0) {
            *out_cluster = current;
            return 0;
        }

        if (status < 0 || component_to_short_name(component, short_name) != 0) {
            return -1;
        }

        status = find_entry_in_directory(current, short_name, &match, &free_sector, &free_offset);
        if (status != 1) {
            return -1;
        }

        if ((match.entry[11] & FAT32_ATTR_DIRECTORY) == 0U) {
            return -1;
        }

        current = entry_cluster(match.entry);
        if (current < 2U) {
            return -1;
        }
    }
}

static int resolve_parent_and_leaf(const char* abs_path, uint32_t* out_parent_cluster, uint8_t out_leaf[11]) {
    const char* cursor = abs_path;
    uint32_t current = fs.root_cluster;
    char component[13];
    char next_component[13];
    int has_next;

    if (abs_path == NULL || abs_path[0] != '/' || abs_path[1] == '\0') {
        return -1;
    }

    if (next_path_component(&cursor, component, (int)sizeof(component)) <= 0) {
        return -1;
    }

    while (1) {
        const char* lookahead = cursor;
        has_next = next_path_component(&lookahead, next_component, (int)sizeof(next_component));
        if (has_next <= 0) {
            if (component_to_short_name(component, out_leaf) != 0) {
                return -1;
            }

            *out_parent_cluster = current;
            return 0;
        }

        {
            DirEntryRef match;
            uint32_t free_sector = 0;
            uint32_t free_offset = 0;
            uint8_t short_name[11];
            if (component_to_short_name(component, short_name) != 0) {
                return -1;
            }

            if (find_entry_in_directory(current, short_name, &match, &free_sector, &free_offset) != 1) {
                return -1;
            }

            if ((match.entry[11] & FAT32_ATTR_DIRECTORY) == 0U) {
                return -1;
            }

            current = entry_cluster(match.entry);
            if (current < 2U) {
                return -1;
            }
        }

        cursor = lookahead;
        while (*cursor == '/') {
            cursor++;
        }
        {
            int i = 0;
            while (next_component[i] != '\0') {
                component[i] = next_component[i];
                i++;
            }
            component[i] = '\0';
        }
    }
}

static int write_directory_entry(uint32_t sector_number, uint32_t offset, const uint8_t entry[32]) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];

    if (blockdev_read_sector(sector_number, sector) != 0) {
        return -1;
    }

    memory_copy(&sector[offset], entry, 32U);

    if (blockdev_write_sector(sector_number, sector) != 0) {
        return -1;
    }

    return 0;
}

static int create_entry_in_directory(
    uint32_t parent_cluster,
    const uint8_t short_name[11],
    uint8_t attributes,
    uint32_t first_cluster,
    uint32_t size
) {
    DirEntryRef match;
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    uint8_t new_entry[32];
    int status = find_entry_in_directory(parent_cluster, short_name, &match, &free_sector, &free_offset);

    if (status != 0) {
        return -1;
    }

    memory_set(new_entry, 0, sizeof(new_entry));
    memory_copy(new_entry, short_name, 11U);
    new_entry[11] = attributes;
    set_entry_cluster(new_entry, first_cluster);
    write_u32le(&new_entry[28], size);

    return write_directory_entry(free_sector, free_offset, new_entry);
}

static int find_path_entry(const char* abs_path, uint32_t* out_parent_cluster, DirEntryRef* out_entry) {
    uint8_t leaf[11];
    uint32_t parent;
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;

    if (resolve_parent_and_leaf(abs_path, &parent, leaf) != 0) {
        return -1;
    }

    if (find_entry_in_directory(parent, leaf, out_entry, &free_sector, &free_offset) != 1) {
        return -1;
    }

    *out_parent_cluster = parent;
    return 0;
}

int fat32_mount(void) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];

    memory_set((uint8_t*)&fs, 0, (uint32_t)sizeof(fs));

    if (blockdev_read_sector(0, sector) != 0) {
        ERROR_LOG("failed to read FAT32 boot sector");
        return -1;
    }

    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        ERROR_LOG("invalid FAT boot signature");
        return -1;
    }

    fs.bytes_per_sector = read_u16le(&sector[11]);
    fs.sectors_per_cluster = sector[13];
    fs.reserved_sectors = read_u16le(&sector[14]);
    fs.fat_count = sector[16];
    fs.total_sectors = read_u32le(&sector[32]);
    fs.sectors_per_fat = read_u32le(&sector[36]);
    fs.root_cluster = read_u32le(&sector[44]);

    if (fs.bytes_per_sector != BLOCKDEV_SECTOR_SIZE || fs.sectors_per_cluster == 0 || fs.fat_count == 0 || fs.sectors_per_fat == 0 || fs.root_cluster < 2U) {
        ERROR_LOG("unsupported FAT32 geometry");
        return -1;
    }

    fs.fat_start_sector = (uint32_t)fs.reserved_sectors;
    fs.data_start_sector = fs.fat_start_sector + ((uint32_t)fs.fat_count * fs.sectors_per_fat);

    if (fs.data_start_sector >= blockdev_sector_count() || fs.total_sectors > blockdev_sector_count() || fs.sectors_per_cluster == 0) {
        ERROR_LOG("FAT32 layout exceeds device bounds");
        return -1;
    }

    fs.mounted = 1;
    DEBUG_LOG("FAT32 mounted");
    return 0;
}

int fat32_is_mounted(void) {
    return fs.mounted;
}

void fat32_unmount(void) {
    fs.mounted = 0;
}

int fat32_list_dir(const char* abs_path, fat32_list_visitor_t visitor, void* context) {
    uint32_t cluster;
    uint32_t guard = 0;

    if (!fs.mounted || visitor == NULL || abs_path == NULL) {
        return -1;
    }

    if (resolve_directory_cluster(abs_path, &cluster) != 0) {
        return -1;
    }

    while (cluster >= 2U && cluster < 0x0FFFFFF8U) {
        uint32_t start_sector = cluster_to_sector(cluster);

        for (uint32_t s = 0; s < (uint32_t)fs.sectors_per_cluster; s++) {
            uint8_t sector[BLOCKDEV_SECTOR_SIZE];

            if (blockdev_read_sector(start_sector + s, sector) != 0) {
                ERROR_LOG("failed to read directory cluster");
                return -1;
            }

            for (uint32_t offset = 0; offset < BLOCKDEV_SECTOR_SIZE; offset += 32U) {
                const uint8_t* entry = &sector[offset];
                uint8_t first = entry[0];
                uint8_t attr = entry[11];

                if (first == 0x00) {
                    return 0;
                }

                if (first == 0xE5 || attr == 0x0F || (attr & 0x08U) != 0) {
                    continue;
                }

                fat32_dir_entry_t out = {{0}, 0, 0};
                if (make_display_name(entry, out.name) != 0) {
                    continue;
                }

                out.is_directory = (uint8_t)(((attr & 0x10U) != 0U) ? 1U : 0U);
                out.size = read_u32le(&entry[28]);

                if (visitor(&out, context) != 0) {
                    return 0;
                }
            }
        }

        cluster = read_fat_entry(cluster);
        guard++;
        if (guard > 4096U) {
            ERROR_LOG("FAT32 directory traversal guard triggered");
            return -1;
        }
    }

    return 0;
}

int fat32_touch_file(const char* abs_path) {
    uint8_t leaf[11];
    uint32_t parent;
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    DirEntryRef match;

    if (!fs.mounted || resolve_parent_and_leaf(abs_path, &parent, leaf) != 0) {
        return -1;
    }

    if (find_entry_in_directory(parent, leaf, &match, &free_sector, &free_offset) == 1) {
        if ((match.entry[11] & FAT32_ATTR_DIRECTORY) != 0U) {
            return -1;
        }

        return 0;
    }

    return create_entry_in_directory(parent, leaf, 0x20U, 0, 0);
}

int fat32_write_file(const char* abs_path, const char* data, uint32_t size, int append) {
    uint32_t bytes_per_cluster;
    uint32_t final_size;
    uint32_t required_clusters;
    uint32_t parent;
    DirEntryRef ref;
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    uint8_t leaf[11];
    uint32_t original_size;
    uint32_t first_cluster;
    uint32_t write_cluster;
    uint32_t write_offset;
    uint32_t copied = 0;

    if (!fs.mounted || (size > 0U && data == NULL) || resolve_parent_and_leaf(abs_path, &parent, leaf) != 0) {
        return -1;
    }

    if (find_entry_in_directory(parent, leaf, &ref, &free_sector, &free_offset) != 1) {
        if (create_entry_in_directory(parent, leaf, 0x20U, 0, 0) != 0) {
            return -1;
        }

        if (find_entry_in_directory(parent, leaf, &ref, &free_sector, &free_offset) != 1) {
            return -1;
        }
    }

    if ((ref.entry[11] & FAT32_ATTR_DIRECTORY) != 0U) {
        return -1;
    }

    original_size = read_u32le(&ref.entry[28]);
    first_cluster = entry_cluster(ref.entry);
    bytes_per_cluster = cluster_bytes();

    if (append) {
        if (size > (0xFFFFFFFFU - original_size)) {
            return -1;
        }

        final_size = original_size + size;
    } else {
        final_size = size;
    }

    if (!append) {
        if (is_data_cluster(first_cluster)) {
            if (free_cluster_chain(first_cluster) != 0) {
                return -1;
            }
        }

        first_cluster = 0;
        original_size = 0;
    }

    if (final_size == 0U) {
        set_entry_cluster(ref.entry, 0);
        write_u32le(&ref.entry[28], 0);
        return write_directory_entry(ref.sector, ref.offset, ref.entry);
    }

    required_clusters = (final_size + bytes_per_cluster - 1U) / bytes_per_cluster;
    if (ensure_file_cluster_chain(&first_cluster, required_clusters) != 0) {
        return -1;
    }

    write_cluster = first_cluster;
    write_offset = append ? original_size : 0U;

    while (write_offset >= bytes_per_cluster) {
        write_cluster = read_fat_entry(write_cluster);
        if (!is_data_cluster(write_cluster)) {
            return -1;
        }

        write_offset -= bytes_per_cluster;
    }

    while (copied < size) {
        uint32_t chunk = bytes_per_cluster - write_offset;
        if (chunk > (size - copied)) {
            chunk = size - copied;
        }

        if (write_cluster_bytes(write_cluster, write_offset, (const uint8_t*)data + copied, chunk) != 0) {
            return -1;
        }

        copied += chunk;
        write_offset = 0U;

        if (copied < size) {
            write_cluster = read_fat_entry(write_cluster);
            if (!is_data_cluster(write_cluster)) {
                return -1;
            }
        }
    }

    set_entry_cluster(ref.entry, first_cluster);
    write_u32le(&ref.entry[28], final_size);
    return write_directory_entry(ref.sector, ref.offset, ref.entry);
}

int fat32_read_file(const char* abs_path, char* out, uint32_t out_capacity, uint32_t* out_size) {
    uint32_t parent;
    DirEntryRef ref;
    uint32_t size;
    uint32_t cluster;
    uint32_t bytes_per_cluster;
    uint32_t copied = 0;
    uint32_t remaining;
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    uint8_t leaf[11];

    if (!fs.mounted || out == NULL || out_capacity == 0 || out_size == NULL || resolve_parent_and_leaf(abs_path, &parent, leaf) != 0) {
        return -1;
    }

    if (find_entry_in_directory(parent, leaf, &ref, &free_sector, &free_offset) != 1) {
        return -1;
    }

    if ((ref.entry[11] & FAT32_ATTR_DIRECTORY) != 0U) {
        return -1;
    }

    size = read_u32le(&ref.entry[28]);
    cluster = entry_cluster(ref.entry);

    if (size >= out_capacity) {
        return -1;
    }

    if (size == 0U) {
        out[0] = '\0';
        *out_size = 0;
        return 0;
    }

    if (!is_data_cluster(cluster)) {
        return -1;
    }

    bytes_per_cluster = cluster_bytes();
    remaining = size;

    while (remaining > 0U) {
        uint32_t chunk = bytes_per_cluster;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (read_cluster_bytes(cluster, 0U, (uint8_t*)out + copied, chunk) != 0) {
            return -1;
        }

        copied += chunk;
        remaining -= chunk;

        if (remaining > 0U) {
            cluster = read_fat_entry(cluster);
            if (!is_data_cluster(cluster)) {
                return -1;
            }
        }
    }

    out[size] = '\0';
    *out_size = size;
    return 0;
}

int fat32_make_dir(const char* abs_path) {
    uint8_t leaf[11];
    uint32_t parent;
    uint32_t new_cluster;
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    DirEntryRef match;

    if (!fs.mounted || resolve_parent_and_leaf(abs_path, &parent, leaf) != 0) {
        return -1;
    }

    if (find_entry_in_directory(parent, leaf, &match, &free_sector, &free_offset) == 1) {
        return -1;
    }

    if (allocate_cluster(&new_cluster) != 0) {
        return -1;
    }

    memory_set(sector, 0, sizeof(sector));
    memory_copy(&sector[0], (const uint8_t*)".          ", 11U);
    sector[11] = FAT32_ATTR_DIRECTORY;
    set_entry_cluster(&sector[0], new_cluster);

    memory_copy(&sector[32], (const uint8_t*)"..         ", 11U);
    sector[32 + 11] = FAT32_ATTR_DIRECTORY;
    set_entry_cluster(&sector[32], parent);

    if (blockdev_write_sector(cluster_to_sector(new_cluster), sector) != 0) {
        return -1;
    }

    return create_entry_in_directory(parent, leaf, FAT32_ATTR_DIRECTORY, new_cluster, 0);
}

int fat32_remove_file(const char* abs_path) {
    uint32_t parent;
    DirEntryRef ref;
    uint32_t cluster;

    if (!fs.mounted || find_path_entry(abs_path, &parent, &ref) != 0) {
        return -1;
    }

    (void)parent;

    if ((ref.entry[11] & FAT32_ATTR_DIRECTORY) != 0U) {
        return -1;
    }

    cluster = entry_cluster(ref.entry);
    if (cluster >= 2U && free_cluster_chain(cluster) != 0) {
        return -1;
    }

    ref.entry[0] = 0xE5;
    return write_directory_entry(ref.sector, ref.offset, ref.entry);
}

int fat32_remove_dir(const char* abs_path) {
    uint32_t parent;
    DirEntryRef ref;
    uint32_t cluster;
    uint32_t guard = 0;

    if (!fs.mounted || abs_path == NULL || abs_path[0] != '/' || abs_path[1] == '\0') {
        return -1;
    }

    if (find_path_entry(abs_path, &parent, &ref) != 0) {
        return -1;
    }

    (void)parent;

    if ((ref.entry[11] & FAT32_ATTR_DIRECTORY) == 0U) {
        return -1;
    }

    cluster = entry_cluster(ref.entry);
    while (cluster >= 2U && cluster < 0x0FFFFFF8U) {
        uint32_t start_sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < (uint32_t)fs.sectors_per_cluster; s++) {
            uint8_t sector[BLOCKDEV_SECTOR_SIZE];
            if (blockdev_read_sector(start_sector + s, sector) != 0) {
                return -1;
            }

            for (uint32_t offset = 0; offset < BLOCKDEV_SECTOR_SIZE; offset += 32U) {
                uint8_t first = sector[offset];
                uint8_t attr = sector[offset + 11U];

                if (first == 0x00U) {
                    break;
                }

                if (first == 0xE5U || attr == FAT32_ATTR_LFN) {
                    continue;
                }

                if (offset == 0U || offset == 32U) {
                    continue;
                }

                return -1;
            }
        }

        cluster = read_fat_entry(cluster);
        guard++;
        if (guard > 4096U) {
            return -1;
        }
    }

    cluster = entry_cluster(ref.entry);
    if (cluster >= 2U && free_cluster_chain(cluster) != 0) {
        return -1;
    }

    ref.entry[0] = 0xE5;
    return write_directory_entry(ref.sector, ref.offset, ref.entry);
}

int fat32_path_is_dir(const char* abs_path, int* out_is_dir) {
    uint32_t parent;
    DirEntryRef ref;

    if (!fs.mounted || out_is_dir == NULL || abs_path == NULL) {
        return -1;
    }

    if (abs_path[0] == '/' && abs_path[1] == '\0') {
        *out_is_dir = 1;
        return 0;
    }

    if (find_path_entry(abs_path, &parent, &ref) != 0) {
        return -1;
    }

    (void)parent;

    *out_is_dir = ((ref.entry[11] & FAT32_ATTR_DIRECTORY) != 0U) ? 1 : 0;
    return 0;
}

int fat32_get_space(uint32_t* out_total_sectors, uint32_t* out_used_sectors, uint32_t* out_free_sectors) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t cluster_limit;
    uint32_t free_clusters = 0U;

    if (!fs.mounted || out_total_sectors == NULL || out_used_sectors == NULL || out_free_sectors == NULL) {
        return -1;
    }

    cluster_limit = max_cluster_index();
    if (cluster_limit <= 2U) {
        return -1;
    }

    for (uint32_t fat_sector = 0; fat_sector < fs.sectors_per_fat; fat_sector++) {
        uint32_t first_cluster = (fat_sector * BLOCKDEV_SECTOR_SIZE) / 4U;
        if (first_cluster >= cluster_limit) {
            break;
        }

        if (blockdev_read_sector(fs.fat_start_sector + fat_sector, sector) != 0) {
            return -1;
        }

        for (uint32_t offset = 0; offset < BLOCKDEV_SECTOR_SIZE; offset += 4U) {
            uint32_t cluster = first_cluster + offset / 4U;
            if (cluster >= 2U && cluster < cluster_limit && read_u32le(&sector[offset]) == FAT32_CLUSTER_FREE) {
                free_clusters++;
            }
        }
    }

    {
        uint32_t total_clusters = cluster_limit - 2U;
        uint32_t used_clusters = total_clusters - free_clusters;
        *out_total_sectors = total_clusters * (uint32_t)fs.sectors_per_cluster;
        *out_used_sectors = used_clusters * (uint32_t)fs.sectors_per_cluster;
        *out_free_sectors = free_clusters * (uint32_t)fs.sectors_per_cluster;
    }

    return 0;
}

static int fat32_shrink_tail_is_free(uint32_t new_cluster_limit, uint32_t old_cluster_limit) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];

    for (uint32_t fat_sector = 0U; fat_sector < fs.sectors_per_fat; fat_sector++) {
        uint32_t first_cluster = (fat_sector * BLOCKDEV_SECTOR_SIZE) / 4U;
        if (first_cluster >= old_cluster_limit) {
            break;
        }
        if (blockdev_read_sector(fs.fat_start_sector + fat_sector, sector) != 0) {
            return FAT32_RESIZE_ERROR_IO;
        }

        for (uint32_t offset = 0U; offset < BLOCKDEV_SECTOR_SIZE; offset += 4U) {
            uint32_t cluster = first_cluster + offset / 4U;
            uint32_t next_cluster = read_u32le(&sector[offset]) & 0x0FFFFFFFU;
            if (cluster < 2U || cluster >= old_cluster_limit) {
                continue;
            }
            if (cluster >= new_cluster_limit) {
                if (next_cluster != FAT32_CLUSTER_FREE) {
                    return FAT32_RESIZE_ERROR_DATA_PRESENT;
                }
            } else if (next_cluster >= new_cluster_limit && next_cluster < FAT32_CLUSTER_EOC_MIN) {
                return FAT32_RESIZE_ERROR_DATA_PRESENT;
            }
        }
    }

    return FAT32_RESIZE_OK;
}

static int fat32_invalidate_fsinfo(uint32_t sector_index) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];

    if (sector_index >= fs.reserved_sectors) {
        return FAT32_RESIZE_ERROR_INVALID;
    }
    if (blockdev_read_sector(sector_index, sector) != 0) {
        return FAT32_RESIZE_ERROR_IO;
    }
    if (read_u32le(&sector[0U]) != 0x41615252U
        || read_u32le(&sector[484U]) != 0x61417272U
        || read_u32le(&sector[508U]) != 0xAA550000U) {
        return FAT32_RESIZE_OK;
    }

    write_u32le(&sector[488U], 0xFFFFFFFFU);
    write_u32le(&sector[492U], 0xFFFFFFFFU);
    return blockdev_write_sector(sector_index, sector) == 0
        ? FAT32_RESIZE_OK : FAT32_RESIZE_ERROR_IO;
}

int fat32_shrink_volume(uint32_t new_total_sectors) {
    uint8_t primary[BLOCKDEV_SECTOR_SIZE];
    uint8_t backup[BLOCKDEV_SECTOR_SIZE];
    uint32_t data_sectors;
    uint32_t cluster_count;
    uint32_t new_cluster_limit;
    uint32_t old_cluster_limit;
    uint16_t backup_sector;
    uint16_t fsinfo_sector;
    int have_backup;

    if (!fs.mounted || new_total_sectors <= fs.data_start_sector
        || new_total_sectors > blockdev_sector_count()) {
        return FAT32_RESIZE_ERROR_INVALID;
    }
    if (new_total_sectors == fs.total_sectors) {
        return FAT32_RESIZE_OK;
    }
    if (new_total_sectors > fs.total_sectors) {
        return FAT32_RESIZE_ERROR_INVALID;
    }

    data_sectors = new_total_sectors - fs.data_start_sector;
    cluster_count = data_sectors / (uint32_t)fs.sectors_per_cluster;
    if (cluster_count < FAT32_MIN_CLUSTER_COUNT || cluster_count > 0x0FFFFFF0U - 2U) {
        return FAT32_RESIZE_ERROR_INVALID;
    }
    new_cluster_limit = clamp_cluster_limit(2U + cluster_count);
    old_cluster_limit = max_cluster_index();
    if (new_cluster_limit <= fs.root_cluster || new_cluster_limit > old_cluster_limit) {
        return FAT32_RESIZE_ERROR_INVALID;
    }

    {
        int validation = fat32_shrink_tail_is_free(new_cluster_limit, old_cluster_limit);
        if (validation != FAT32_RESIZE_OK) {
            return validation;
        }
    }

    if (blockdev_read_sector(0U, primary) != 0
        || read_u32le(&primary[32U]) != fs.total_sectors) {
        return FAT32_RESIZE_ERROR_IO;
    }
    backup_sector = read_u16le(&primary[50U]);
    have_backup = backup_sector != 0U && backup_sector < fs.reserved_sectors
        && backup_sector < fs.total_sectors;
    if (have_backup && blockdev_read_sector(backup_sector, backup) != 0) {
        return FAT32_RESIZE_ERROR_IO;
    }

    fsinfo_sector = read_u16le(&primary[48U]);
    if (fsinfo_sector != 0U && fsinfo_sector != 0xFFFFU) {
        int fsinfo_result = fat32_invalidate_fsinfo(fsinfo_sector);
        if (fsinfo_result != FAT32_RESIZE_OK) {
            return fsinfo_result;
        }
    }
    if (have_backup) {
        uint16_t backup_fsinfo = read_u16le(&backup[48U]);
        uint32_t backup_fsinfo_sector = (uint32_t)backup_sector + backup_fsinfo;
        if (backup_fsinfo != 0U && backup_fsinfo != 0xFFFFU
            && backup_fsinfo_sector != fsinfo_sector
            && backup_fsinfo_sector < fs.reserved_sectors) {
            int fsinfo_result = fat32_invalidate_fsinfo(backup_fsinfo_sector);
            if (fsinfo_result != FAT32_RESIZE_OK) {
                return fsinfo_result;
            }
        }
    }

    write_u32le(&primary[32U], new_total_sectors);
    if (have_backup) {
        write_u32le(&backup[32U], new_total_sectors);
        if (blockdev_write_sector(backup_sector, backup) != 0) {
            return FAT32_RESIZE_ERROR_IO;
        }
    }
    if (blockdev_write_sector(0U, primary) != 0) {
        if (have_backup) {
            write_u32le(&backup[32U], fs.total_sectors);
            (void)blockdev_write_sector(backup_sector, backup);
        }
        return FAT32_RESIZE_ERROR_IO;
    }

    fs.total_sectors = new_total_sectors;
    return FAT32_RESIZE_OK;
}

static int fat32_prepare_volume_label(const char* label, uint8_t out_label[11]) {
    uint32_t length = 0U;

    if (label == NULL || label[0] == '\0') {
        return -1;
    }

    while (label[length] != '\0') {
        uint8_t character = (uint8_t)label[length];
        if (length >= 11U) {
            return -1;
        }
        if (!((character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-')) {
            return -1;
        }
        out_label[length] = to_upper(character);
        length++;
    }

    while (length < 11U) {
        out_label[length++] = ' ';
    }

    return 0;
}

int fat32_format_volume(const char* label, uint32_t hidden_sectors) {
    const uint32_t reserved_sectors = 32U;
    const uint32_t fat_count = 2U;
    const uint32_t sectors_per_cluster = 1U;
    uint8_t boot_sector[BLOCKDEV_SECTOR_SIZE];
    uint8_t fsinfo_sector[BLOCKDEV_SECTOR_SIZE];
    uint8_t fat_sector[BLOCKDEV_SECTOR_SIZE];
    uint8_t label_bytes[11];
    uint32_t total_sectors = blockdev_sector_count();
    uint32_t sectors_per_fat = 1U;
    uint32_t data_clusters = 0U;
    int converged = 0;

    if (total_sectors <= reserved_sectors + fat_count * 2U
        || fat32_prepare_volume_label(label, label_bytes) != 0) {
        return FAT32_RESIZE_ERROR_INVALID;
    }

    for (uint32_t iteration = 0U; iteration < 16U; iteration++) {
        uint32_t data_sectors;
        uint32_t required_fat_sectors;
        if (sectors_per_fat > (total_sectors - reserved_sectors) / fat_count) {
            return FAT32_RESIZE_ERROR_INVALID;
        }
        data_sectors = total_sectors - reserved_sectors - fat_count * sectors_per_fat;
        data_clusters = data_sectors / sectors_per_cluster;
        required_fat_sectors = ((data_clusters + 2U) * 4U + BLOCKDEV_SECTOR_SIZE - 1U)
            / BLOCKDEV_SECTOR_SIZE;
        if (required_fat_sectors == sectors_per_fat) {
            converged = 1;
            break;
        }
        sectors_per_fat = required_fat_sectors;
    }

    /* PrismOS accepts small FAT32-layout volumes even below the spec's cluster-count threshold. */
    if (!converged || data_clusters == 0U
        || data_clusters > 0x0FFFFFF0U - 2U
        || (data_clusters + 2U) > sectors_per_fat * (BLOCKDEV_SECTOR_SIZE / 4U)) {
        return FAT32_RESIZE_ERROR_INVALID;
    }

    /* Quick format: rebuild only filesystem metadata; leave file-data sectors untouched. */
    fat32_unmount();
    for (uint32_t fat_index = 0U; fat_index < fat_count; fat_index++) {
        uint32_t fat_start = reserved_sectors + fat_index * sectors_per_fat;
        for (uint32_t sector_index = 0U; sector_index < sectors_per_fat; sector_index++) {
            if (sector_index == 0U) {
                memory_set(fat_sector, 0U, sizeof(fat_sector));
                fat_sector[0] = 0xF8U;
                fat_sector[1] = 0xFFU;
                fat_sector[2] = 0xFFU;
                fat_sector[3] = 0x0FU;
                fat_sector[4] = 0xFFU;
                fat_sector[5] = 0xFFU;
                fat_sector[6] = 0xFFU;
                fat_sector[7] = 0x0FU;
                fat_sector[8] = 0xFFU;
                fat_sector[9] = 0xFFU;
                fat_sector[10] = 0xFFU;
                fat_sector[11] = 0x0FU;
            } else {
                memory_set(fat_sector, 0U, sizeof(fat_sector));
            }
            if (blockdev_write_sector(fat_start + sector_index, fat_sector) != 0) {
                return FAT32_RESIZE_ERROR_IO;
            }
        }
    }

    memory_set(fsinfo_sector, 0U, sizeof(fsinfo_sector));
    write_u32le(&fsinfo_sector[0U], 0x41615252U);
    write_u32le(&fsinfo_sector[484U], 0x61417272U);
    write_u32le(&fsinfo_sector[488U], data_clusters - 1U);
    write_u32le(&fsinfo_sector[492U], 3U);
    write_u32le(&fsinfo_sector[508U], 0xAA550000U);
    if (blockdev_write_sector(1U, fsinfo_sector) != 0
        || blockdev_write_sector(7U, fsinfo_sector) != 0) {
        return FAT32_RESIZE_ERROR_IO;
    }

    memory_set(fat_sector, 0U, sizeof(fat_sector));
    for (uint32_t index = 0U; index < 11U; index++) {
        fat_sector[index] = label_bytes[index];
    }
    fat_sector[11U] = FAT32_ATTR_VOLUME_ID;
    if (blockdev_write_sector(reserved_sectors + fat_count * sectors_per_fat, fat_sector) != 0) {
        return FAT32_RESIZE_ERROR_IO;
    }

    memory_set(boot_sector, 0U, sizeof(boot_sector));
    boot_sector[0U] = 0xEBU;
    boot_sector[1U] = 0x58U;
    boot_sector[2U] = 0x90U;
    boot_sector[3U] = 'P';
    boot_sector[4U] = 'R';
    boot_sector[5U] = 'I';
    boot_sector[6U] = 'S';
    boot_sector[7U] = 'M';
    boot_sector[8U] = 'O';
    boot_sector[9U] = 'S';
    boot_sector[10U] = ' ';
    write_u16le(&boot_sector[11U], BLOCKDEV_SECTOR_SIZE);
    boot_sector[13U] = (uint8_t)sectors_per_cluster;
    write_u16le(&boot_sector[14U], (uint16_t)reserved_sectors);
    boot_sector[16U] = (uint8_t)fat_count;
    write_u16le(&boot_sector[17U], 0U);
    write_u16le(&boot_sector[19U], 0U);
    boot_sector[21U] = 0xF8U;
    write_u16le(&boot_sector[22U], 0U);
    write_u16le(&boot_sector[24U], 63U);
    write_u16le(&boot_sector[26U], 255U);
    write_u32le(&boot_sector[28U], hidden_sectors);
    write_u32le(&boot_sector[32U], total_sectors);
    write_u32le(&boot_sector[36U], sectors_per_fat);
    write_u16le(&boot_sector[40U], 0U);
    write_u16le(&boot_sector[42U], 0U);
    write_u32le(&boot_sector[44U], 2U);
    write_u16le(&boot_sector[48U], 1U);
    write_u16le(&boot_sector[50U], 6U);
    boot_sector[64U] = 0x80U;
    boot_sector[66U] = 0x29U;
    write_u32le(&boot_sector[67U], hidden_sectors ^ total_sectors ^ 0x50524953U);
    for (uint32_t index = 0U; index < 11U; index++) {
        boot_sector[71U + index] = label_bytes[index];
    }
    boot_sector[82U] = 'F';
    boot_sector[83U] = 'A';
    boot_sector[84U] = 'T';
    boot_sector[85U] = '3';
    boot_sector[86U] = '2';
    boot_sector[87U] = ' ';
    boot_sector[88U] = ' ';
    boot_sector[89U] = ' ';
    boot_sector[510U] = 0x55U;
    boot_sector[511U] = 0xAAU;

    if (blockdev_write_sector(6U, boot_sector) != 0
        || blockdev_write_sector(0U, boot_sector) != 0) {
        return FAT32_RESIZE_ERROR_IO;
    }

    return FAT32_RESIZE_OK;
}

static int fat32_update_root_volume_entry(const uint8_t label_bytes[11]) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t cluster = fs.root_cluster;
    uint32_t guard = 0U;
    uint32_t limit = max_cluster_index();

    /* A corrupt cyclic directory chain must not make a label update loop forever. */
    while (is_data_cluster(cluster) && cluster < limit && guard++ < 1024U) {
        uint32_t start_sector = cluster_to_sector(cluster);
        for (uint32_t sector_index = 0; sector_index < (uint32_t)fs.sectors_per_cluster; sector_index++) {
            if (blockdev_read_sector(start_sector + sector_index, sector) != 0) {
                return -1;
            }

            for (uint32_t offset = 0; offset < BLOCKDEV_SECTOR_SIZE; offset += 32U) {
                uint8_t* entry = &sector[offset];
                if (entry[0] == 0U) {
                    return 0;
                }
                if (entry[0] != 0xE5U
                    && (entry[11] & FAT32_ATTR_VOLUME_ID) != 0U
                    && entry[11] != FAT32_ATTR_LFN) {
                    for (uint32_t index = 0; index < 11U; index++) {
                        entry[index] = label_bytes[index];
                    }
                    return blockdev_write_sector(start_sector + sector_index, sector);
                }
            }
        }

        cluster = read_fat_entry(cluster);
    }

    return 0;
}

int fat32_get_volume_label(char* out_label, uint32_t out_capacity) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint32_t length = 11U;

    if (!fs.mounted || out_label == NULL || out_capacity < 12U || blockdev_read_sector(0U, sector) != 0) {
        return -1;
    }

    while (length > 0U && (sector[71U + length - 1U] == ' ' || sector[71U + length - 1U] == 0U)) {
        length--;
    }

    for (uint32_t index = 0; index < length; index++) {
        out_label[index] = (char)sector[71U + index];
    }
    out_label[length] = '\0';
    return 0;
}

int fat32_set_volume_label(const char* label) {
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint8_t label_bytes[11];
    uint16_t backup_sector;

    if (!fs.mounted || fat32_prepare_volume_label(label, label_bytes) != 0
        || blockdev_read_sector(0U, sector) != 0) {
        return -1;
    }

    if (fat32_update_root_volume_entry(label_bytes) != 0) {
        return -1;
    }

    backup_sector = read_u16le(&sector[50U]);
    for (uint32_t index = 0; index < sizeof(label_bytes); index++) {
        sector[71U + index] = label_bytes[index];
    }

    if (backup_sector != 0U && backup_sector < fs.total_sectors) {
        uint8_t backup[BLOCKDEV_SECTOR_SIZE];
        if (blockdev_read_sector(backup_sector, backup) != 0) {
            return -1;
        }
        for (uint32_t index = 0; index < sizeof(label_bytes); index++) {
            backup[71U + index] = label_bytes[index];
        }
        if (blockdev_write_sector(backup_sector, backup) != 0) {
            return -1;
        }
    }

    if (blockdev_write_sector(0U, sector) != 0) {
        return -1;
    }

    return 0;
}
