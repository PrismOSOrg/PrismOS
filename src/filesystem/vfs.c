#include "filesystem/vfs.h"

#include <stddef.h>

#include "filesystem/fat32/fat32.h"

typedef struct {
    vfs_list_visitor_t visitor;
    void* context;
} VfsListAdapter;

static int string_equals(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }

        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static int string_length(const char* text) {
    int length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

static int path_push_component(char components[][13], int* count, const char* component) {
    int index = 0;

    if (*count >= 16) {
        return -1;
    }

    while (component[index] != '\0' && index < 12) {
        components[*count][index] = component[index];
        index++;
    }

    if (component[index] != '\0') {
        return -1;
    }

    components[*count][index] = '\0';
    *count = *count + 1;
    return 0;
}

static int load_path_components(const char* path, char components[][13], int* count, int allow_relative) {
    const char* cursor = path;

    if (path == NULL || count == NULL) {
        return -1;
    }

    if (!allow_relative && path[0] != '/') {
        return -1;
    }

    while (*cursor != '\0') {
        char component[13];
        int length = 0;

        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        while (*cursor != '\0' && *cursor != '/') {
            if (length >= 12) {
                return -1;
            }

            component[length++] = *cursor;
            cursor++;
        }

        component[length] = '\0';

        if (string_equals(component, ".")) {
            continue;
        }

        if (string_equals(component, "..")) {
            if (*count > 0) {
                *count = *count - 1;
            }
            continue;
        }

        if (path_push_component(components, count, component) != 0) {
            return -1;
        }
    }

    return 0;
}

int vfs_normalize_path(const char* cwd, const char* input, char* out, uint32_t out_capacity) {
    char components[16][13];
    int count = 0;
    int out_index = 0;

    if (cwd == NULL || input == NULL || out == NULL || out_capacity < 2U) {
        return -1;
    }

    if (input[0] == '/') {
        if (load_path_components(input, components, &count, 0) != 0) {
            return -1;
        }
    } else {
        if (load_path_components(cwd, components, &count, 0) != 0) {
            return -1;
        }

        if (load_path_components(input, components, &count, 1) != 0) {
            return -1;
        }
    }

    out[out_index++] = '/';
    for (int i = 0; i < count; i++) {
        int length = string_length(components[i]);
        if ((uint32_t)(out_index + length + 1) >= out_capacity) {
            return -1;
        }

        for (int j = 0; j < length; j++) {
            out[out_index++] = components[i][j];
        }

        if (i != count - 1) {
            out[out_index++] = '/';
        }
    }

    out[out_index] = '\0';
    return 0;
}

static int vfs_list_adapter(const fat32_dir_entry_t* entry, void* context) {
    VfsListAdapter* adapter = (VfsListAdapter*)context;
    vfs_entry_t vfs_entry;
    int index = 0;

    while (index < (int)(sizeof(vfs_entry.name) - 1U) && entry->name[index] != '\0') {
        vfs_entry.name[index] = entry->name[index];
        index++;
    }

    vfs_entry.name[index] = '\0';
    vfs_entry.is_directory = entry->is_directory;
    vfs_entry.size = entry->size;

    return adapter->visitor(&vfs_entry, adapter->context);
}

int vfs_init(void) {
    return fat32_mount();
}

void vfs_unmount(void) {
    fat32_unmount();
}

int vfs_path_is_dir(const char* abs_path, int* out_is_dir) {
    return fat32_path_is_dir(abs_path, out_is_dir);
}

int vfs_list(const char* path, vfs_list_visitor_t visitor, void* context) {
    VfsListAdapter adapter;

    if (path == NULL || visitor == NULL) {
        return -1;
    }

    if (!fat32_is_mounted()) {
        return -1;
    }

    adapter.visitor = visitor;
    adapter.context = context;
    return fat32_list_dir(path, vfs_list_adapter, &adapter);
}

int vfs_touch(const char* abs_path) {
    return fat32_touch_file(abs_path);
}

int vfs_mkdir(const char* abs_path) {
    return fat32_make_dir(abs_path);
}

int vfs_rm(const char* abs_path) {
    return fat32_remove_file(abs_path);
}

int vfs_rmdir(const char* abs_path) {
    return fat32_remove_dir(abs_path);
}

int vfs_read_file(const char* abs_path, char* out, uint32_t out_capacity, uint32_t* out_size) {
    return fat32_read_file(abs_path, out, out_capacity, out_size);
}

int vfs_write_file(const char* abs_path, const char* text, uint32_t text_size, int append) {
    return fat32_write_file(abs_path, text, text_size, append);
}
