#ifndef PRISMOS_FILESYSTEM_FAT32_H
#define PRISMOS_FILESYSTEM_FAT32_H

#include <stdint.h>

typedef struct {
    char name[13];
    uint8_t is_directory;
    uint32_t size;
} fat32_dir_entry_t;

typedef int (*fat32_list_visitor_t)(const fat32_dir_entry_t* entry, void* context);

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

#endif
