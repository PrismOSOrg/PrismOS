#ifndef PRISMOS_FILESYSTEM_VFS_H
#define PRISMOS_FILESYSTEM_VFS_H

#include <stdint.h>

typedef struct {
    char name[13];
    uint8_t is_directory;
    uint32_t size;
} vfs_entry_t;

typedef int (*vfs_list_visitor_t)(const vfs_entry_t* entry, void* context);

/* VFS keeps the shell-side path handling simple and maps everything onto FAT32. */
int vfs_init(void);
void vfs_unmount(void);
int vfs_normalize_path(const char* cwd, const char* input, char* out, uint32_t out_capacity);
int vfs_path_is_dir(const char* abs_path, int* out_is_dir);
int vfs_list(const char* path, vfs_list_visitor_t visitor, void* context);
int vfs_touch(const char* abs_path);
int vfs_mkdir(const char* abs_path);
int vfs_rm(const char* abs_path);
int vfs_rmdir(const char* abs_path);
int vfs_read_file(const char* abs_path, char* out, uint32_t out_capacity, uint32_t* out_size);
int vfs_write_file(const char* abs_path, const char* text, uint32_t text_size, int append);
int vfs_get_space(uint32_t* out_total_sectors, uint32_t* out_used_sectors, uint32_t* out_free_sectors);
int vfs_get_volume_label(char* out_label, uint32_t out_capacity);
int vfs_set_volume_label(const char* label);

#endif
