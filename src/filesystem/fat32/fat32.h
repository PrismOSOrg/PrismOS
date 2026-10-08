#ifndef PRISMOS_FILESYSTEM_FAT32_H
#define PRISMOS_FILESYSTEM_FAT32_H

#include <stdint.h>

typedef struct {
    char name[13];
    uint8_t is_directory;
    uint32_t size;
} fat32_dir_entry_t;

typedef int (*fat32_list_visitor_t)(const fat32_dir_entry_t* entry, void* context);

enum {
    FAT32_RESIZE_OK = 0,
    FAT32_RESIZE_ERROR_IO = -1,
    FAT32_RESIZE_ERROR_DATA_PRESENT = -2,
    FAT32_RESIZE_ERROR_INVALID = -3,
};

int fat32_mount(void);
int fat32_is_mounted(void);
void fat32_unmount(void);
int fat32_list_dir(const char* abs_path, fat32_list_visitor_t visitor, void* context);
int fat32_touch_file(const char* abs_path);
int fat32_write_file(const char* abs_path, const char* data, uint32_t size, int append);
int fat32_read_file(const char* abs_path, char* out, uint32_t out_capacity, uint32_t* out_size);
int fat32_make_dir(const char* abs_path);
int fat32_remove_file(const char* abs_path);
int fat32_remove_dir(const char* abs_path);
int fat32_path_is_dir(const char* abs_path, int* out_is_dir);
int fat32_get_space(uint32_t* out_total_sectors, uint32_t* out_used_sectors, uint32_t* out_free_sectors);
int fat32_shrink_volume(uint32_t new_total_sectors);
/* Quick-formats the currently selected blockdev window; data sectors are not wiped. */
int fat32_format_volume(const char* label, uint32_t hidden_sectors);
int fat32_get_volume_label(char* out_label, uint32_t out_capacity);
int fat32_set_volume_label(const char* label);

#endif
