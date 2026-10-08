#include "apps/app_manager.h"

#include "apps/app_loader.h"
#include "apps/app_runtime.h"
#include "debug/log.h"
#include "filesystem/vfs.h"
#include "util/string.h"

#define APPS_ROOT_PATH "/APPS"
#define DATA_ROOT_PATH "/DATA"
#define DATA_APPS_PATH "/DATA/APPS"
#define APPS_EDITOR_PATH "/APPS/EDITOR.APP"
#define APPS_IDE_PATH "/APPS/IDE.APP"
#define APPS_BANK_PATH "/APPS/BANK.APP"
static int app_manager_ready = 0;

static int ensure_directory(const char* abs_path) {
    int is_dir = 0;

    if (vfs_path_is_dir(abs_path, &is_dir) == 0) {
        return is_dir ? 0 : -1;
    }

    return vfs_mkdir(abs_path);
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

static int ensure_editor_app_installed(void) {
    app_loader_image_t image;
    uint8_t package[PRISM_APP_HEADER_SIZE + 6U];

    if (app_loader_load_image(APPS_EDITOR_PATH, &image) == 0) {
        return 0;
    }

    write_u32le(&package[0], PRISM_APP_MAGIC);
    write_u16le(&package[4], PRISM_APP_FORMAT_VERSION);
    write_u16le(&package[6], 0U);
    write_u32le(&package[8], 0U);
    write_u32le(&package[12], 6U);
    write_u32le(&package[16], 0U);
    package[20] = 'E';
    package[21] = 'D';
    package[22] = 'I';
    package[23] = 'T';
    package[24] = 'O';
    package[25] = 'R';

    if (vfs_write_file(APPS_EDITOR_PATH, (const char*)package, sizeof(package), 0) != 0) {
        ERROR_LOG("failed to install editor app package");
        return -1;
    }

    DEBUG_LOG("editor app package installed");
    return 0;
}

static int ensure_ide_app_installed(void) {
    app_loader_image_t image;
    uint8_t package[PRISM_APP_HEADER_SIZE + 6U];

    if (app_loader_load_image(APPS_IDE_PATH, &image) == 0) {
        return 0;
    }

    write_u32le(&package[0], PRISM_APP_MAGIC);
    write_u16le(&package[4], PRISM_APP_FORMAT_VERSION);
    write_u16le(&package[6], 0U);
    write_u32le(&package[8], 0U);
    write_u32le(&package[12], 6U);
    write_u32le(&package[16], 0U);
    package[20] = 'I';
    package[21] = 'D';
    package[22] = 'E';
    package[23] = '\0';
    package[24] = '\0';
    package[25] = '\0';

    if (vfs_write_file(APPS_IDE_PATH, (const char*)package, sizeof(package), 0) != 0) {
        ERROR_LOG("failed to install ide app package");
        return -1;
    }

    DEBUG_LOG("ide app package installed");
    return 0;
}

static int ensure_bank_app_installed(void) {
    app_loader_image_t image;
    uint8_t package[PRISM_APP_HEADER_SIZE + 4U];

    if (app_loader_load_image(APPS_BANK_PATH, &image) == 0) {
        return 0;
    }

    write_u32le(&package[0], PRISM_APP_MAGIC);
    write_u16le(&package[4], PRISM_APP_FORMAT_VERSION);
    write_u16le(&package[6], 0U);
    write_u32le(&package[8], 0U);
    write_u32le(&package[12], 4U);
    write_u32le(&package[16], 0U);
    package[20] = 'B';
    package[21] = 'A';
    package[22] = 'N';
    package[23] = 'K';

    if (vfs_write_file(APPS_BANK_PATH, (const char*)package, sizeof(package), 0) != 0) {
        ERROR_LOG("failed to install bank app package");
        return -1;
    }

    DEBUG_LOG("bank app package installed");
    return 0;
}

int app_manager_init(void) {
    if (ensure_directory(DATA_ROOT_PATH) != 0) {
        ERROR_LOG("failed to create /DATA directory");
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_directory(DATA_APPS_PATH) != 0) {
        ERROR_LOG("failed to create /DATA/APPS directory");
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_directory(APPS_ROOT_PATH) != 0) {
        ERROR_LOG("failed to create /APPS directory");
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_editor_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_ide_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }

    if (ensure_bank_app_installed() != 0) {
        app_manager_ready = 0;
        return -1;
    }

    app_manager_ready = 1;
    DEBUG_LOG("app manager initialized");
    return 0;
}

int app_manager_is_ready(void) {
    return app_manager_ready;
}

int app_manager_probe_path(const char* abs_path, prism_app_header_t* out_header) {
    app_loader_image_t image;

    if (!app_manager_ready || abs_path == 0 || out_header == 0) {
        return -1;
    }

    if (app_loader_load_image(abs_path, &image) != 0) {
        return -1;
    }

    *out_header = image.header;
    return 0;
}

int app_manager_run_path(const char* app_abs_path, const char* args) {
    app_loader_image_t image;
    const uint8_t* entry;
    uint32_t entry_size;

    if (!app_manager_ready || app_abs_path == 0) {
        ERROR_LOG("app manager run rejected: not ready or invalid path");
        return -1;
    }

    if (app_loader_load_image(app_abs_path, &image) != 0) {
        ERROR_LOG("app manager failed to load app package");

        if (app_abs_path[0] == '/' && app_abs_path[1] == 'A' && app_abs_path[2] == 'P' && app_abs_path[3] == 'P' && app_abs_path[4] == 'S') {
            int repaired = -1;

            if (strcmp(app_abs_path, APPS_EDITOR_PATH) == 0) {
                repaired = ensure_editor_app_installed();
            } else if (strcmp(app_abs_path, APPS_IDE_PATH) == 0) {
                repaired = ensure_ide_app_installed();
            } else if (strcmp(app_abs_path, APPS_BANK_PATH) == 0) {
                repaired = ensure_bank_app_installed();
            }

            if (repaired == 0 && app_loader_load_image(app_abs_path, &image) == 0) {
                DEBUG_LOG("app manager repaired missing built-in app package");
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (image.entry_offset >= image.image_size) {
        ERROR_LOG("app manager invalid app entry offset");
        return -1;
    }

    if ((image.header.flags & PRISM_APP_FLAG_DRIVER) != 0U) {
        ERROR_LOG("driver package rejected by app-run; use driver-run");
        return -1;
    }

    entry = image.image + image.entry_offset;
    entry_size = image.image_size - image.entry_offset;
    DEBUG_LOG("app manager invoking runtime");
    return app_runtime_run(entry, entry_size, args == 0 ? "" : args);
}

int app_manager_run_driver_path(const char* driver_abs_path) {
    app_loader_image_t image;

    if (!app_manager_ready || driver_abs_path == 0) {
        ERROR_LOG("driver manager rejected invalid path or unavailable filesystem");
        return -1;
    }

    if (app_loader_load_image(driver_abs_path, &image) != 0
        || (image.header.flags & PRISM_APP_FLAG_DRIVER) == 0U
        || image.entry_offset >= image.image_size) {
        ERROR_LOG("driver package failed validation or is not marked as a driver");
        return -1;
    }

    DEBUG_LOG("driver package loaded; entering cooperative runtime");
    return app_runtime_run_driver(image.image + image.entry_offset,
        image.image_size - image.entry_offset,
        "");
}

int app_manager_run_editor(const char* target_abs_path) {
    if (!app_manager_ready || target_abs_path == 0) {
        ERROR_LOG("app manager editor launch rejected");
        return -1;
    }

    DEBUG_LOG("app manager launching editor app");
    return app_manager_run_path(APPS_EDITOR_PATH, target_abs_path);
}

int app_manager_run_ide(const char* target_abs_path) {
    if (!app_manager_ready || target_abs_path == 0) {
        ERROR_LOG("app manager ide launch rejected");
        return -1;
    }

    DEBUG_LOG("app manager launching ide app");
    return app_manager_run_path(APPS_IDE_PATH, target_abs_path);
}
