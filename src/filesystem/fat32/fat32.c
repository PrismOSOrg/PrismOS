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
