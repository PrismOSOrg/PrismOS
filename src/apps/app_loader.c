#include "apps/app_loader.h"

#include "debug/log.h"
#include "filesystem/vfs.h"

#define APP_LOADER_MAX_IMAGE_SIZE (96U * 1024U)

static uint8_t app_image_buffer[APP_LOADER_MAX_IMAGE_SIZE + 1U];

static uint16_t read_u16le(const uint8_t* source) {
    return (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
}

static uint32_t read_u32le(const uint8_t* source) {
    return (uint32_t)source[0]
        | ((uint32_t)source[1] << 8)
        | ((uint32_t)source[2] << 16)
        | ((uint32_t)source[3] << 24);
}

static int parse_prism_header(const uint8_t* bytes, uint32_t size, prism_app_header_t* out_header) {
    if (bytes == 0 || out_header == 0 || size < PRISM_APP_HEADER_SIZE) {
        return -1;
    }

    out_header->magic = read_u32le(&bytes[0]);
    out_header->version = read_u16le(&bytes[4]);
    out_header->flags = read_u16le(&bytes[6]);
    out_header->entry_offset = read_u32le(&bytes[8]);
    out_header->image_size = read_u32le(&bytes[12]);
    out_header->reserved = read_u32le(&bytes[16]);

    if (out_header->magic != PRISM_APP_MAGIC || out_header->version != PRISM_APP_FORMAT_VERSION) {
        ERROR_LOG("app loader rejected invalid Prism app header");
        return -1;
    }

    if (out_header->image_size > (size - PRISM_APP_HEADER_SIZE)) {
        ERROR_LOG("app loader rejected truncated Prism app image");
        return -1;
    }

    if (out_header->entry_offset >= out_header->image_size) {
        ERROR_LOG("app loader rejected invalid Prism app entry offset");
        return -1;
    }

    return 0;
}

int app_loader_load_image(const char* abs_path, app_loader_image_t* out_image) {
    uint32_t file_size = 0;
    prism_app_header_t header;

    if (abs_path == 0 || out_image == 0) {
        return -1;
    }

    if (vfs_read_file(abs_path, (char*)app_image_buffer, sizeof(app_image_buffer), &file_size) != 0) {
        ERROR_LOG("app loader could not read app file");
        return -1;
    }

    if (parse_prism_header(app_image_buffer, file_size, &header) != 0) {
        return -1;
    }

    out_image->image = &app_image_buffer[PRISM_APP_HEADER_SIZE];
    out_image->image_size = header.image_size;
    out_image->entry_offset = header.entry_offset;
    out_image->header = header;
    return 0;
}
