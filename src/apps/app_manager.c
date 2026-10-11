#include "apps/app_manager.h"

#include "apps/app_loader.h"
#include "apps/app_runtime.h"
#include "debug/log.h"
#include "display/console.h"
#include "drivers/driver_api.h"
#include "filesystem/vfs.h"
#include "interrupts/interrupts.h"
#include "platform/user_mode.h"
#include "util/string.h"
#include "apps/bytecode_vm.h"

#define APPS_ROOT_PATH "/APPS"
#define DATA_ROOT_PATH "/DATA"
#define DATA_APPS_PATH "/DATA/APPS"
#define APPS_EDITOR_PATH "/APPS/EDITOR.APP"
#define APPS_IDE_PATH "/APPS/IDE.APP"
#define APPS_BANK_PATH "/APPS/BANK.APP"
#define DRIVER_IMAGE_CAPACITY (64U * 1024U)
#define RESIDENT_DRIVER_MAX DRIVER_RUNTIME_MAX

typedef enum {
    RESIDENT_DRIVER_FREE = 0,
    RESIDENT_DRIVER_INITIALIZING,
    RESIDENT_DRIVER_RUNNING,
    RESIDENT_DRIVER_STOPPING
} resident_driver_state_t;

typedef struct {
    uint8_t image[DRIVER_IMAGE_CAPACITY];
    uint32_t image_size;
    uint32_t init_offset;
    uint32_t poll_offset;
    uint32_t shutdown_offset;
    uint32_t service_offset;
    uint32_t runtime_handle;
    resident_driver_state_t state;
    uint8_t callback_active;
} resident_driver_t;

static resident_driver_t resident_drivers[RESIDENT_DRIVER_MAX];
static uint32_t resident_driver_cursor;
static uint32_t resident_driver_last_tick_ms;
static int app_manager_ready = 0;
static int foreground_app_active;

static uint32_t read_u32le(const uint8_t* source) {
    return (uint32_t)source[0]
        | ((uint32_t)source[1] << 8)
        | ((uint32_t)source[2] << 16)
        | ((uint32_t)source[3] << 24);
}

static void resident_driver_release(resident_driver_t* driver) {
    uint32_t handle = driver->runtime_handle;

    DEBUG_LOG("resident driver cleanup starting");
    bytecode_vm_driver_reset(handle);
    bytecode_vm_driver_destroy(handle);
    driver_runtime_destroy(handle);
    driver->state = RESIDENT_DRIVER_FREE;
    driver->callback_active = 0U;
    driver->image_size = 0U;
}

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
    int result;

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
    foreground_app_active = 1;
    result = app_runtime_run(entry, entry_size, args == 0 ? "" : args);
    foreground_app_active = 0;
    return result;
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

    DEBUG_LOG("driver package loaded; entering ring-3 runtime");
    return app_runtime_run_driver(image.image + image.entry_offset,
        image.image_size - image.entry_offset,
        "");
}

int app_manager_start_driver_path(const char* driver_abs_path, uint32_t* out_id) {
    app_loader_image_t package;
    uint32_t table_offset;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t slot;
    int runtime_handle;

    if (!app_manager_ready || driver_abs_path == 0 || out_id == 0
        || app_loader_load_image(driver_abs_path, &package) != 0
        || (package.header.flags & PRISM_APP_FLAG_DRIVER) == 0U
        || package.image_size > DRIVER_IMAGE_CAPACITY
        || package.image_size < BCVM_IMAGE_HEADER_SIZE) {
        return -1;
    }
    if (!user_mode_isolation_available()
        || !bytecode_vm_driver_isolation_available()) {
        ERROR_LOG("resident driver start rejected: ring-3 isolation unavailable");
        return -1;
    }

    if (read_u32le(&package.image[0]) != BCVM_MAGIC) {
        return -1;
    }
    code_size = read_u32le(&package.image[8]);
    data_size = read_u32le(&package.image[12]);
    if (code_size > package.image_size - BCVM_IMAGE_HEADER_SIZE
        || data_size > package.image_size - BCVM_IMAGE_HEADER_SIZE - code_size) {
        return -1;
    }
    table_offset = BCVM_IMAGE_HEADER_SIZE + code_size + data_size;
    if (table_offset > package.image_size
        || package.image_size - table_offset < PRISM_DRIVER_ENTRY_LEGACY_SIZE
        || read_u32le(&package.image[table_offset]) != PRISM_DRIVER_ENTRY_MAGIC) {
        return -1;
    }

    for (slot = 0U; slot < RESIDENT_DRIVER_MAX; ++slot) {
        if (resident_drivers[slot].state == RESIDENT_DRIVER_FREE) {
            break;
        }
    }
    if (slot == RESIDENT_DRIVER_MAX) {
        return -1;
    }

    runtime_handle = driver_runtime_create();
    if (runtime_handle < 0) {
        return -1;
    }

    {
        resident_driver_t* driver = &resident_drivers[slot];
        const uint32_t init_offset = read_u32le(&package.image[table_offset + 4U]);
        const uint32_t poll_offset = read_u32le(&package.image[table_offset + 8U]);
        const uint32_t shutdown_offset = read_u32le(&package.image[table_offset + 12U]);
        const uint32_t service_offset =
            package.image_size - table_offset >= PRISM_DRIVER_ENTRY_TABLE_SIZE
                ? read_u32le(&package.image[table_offset + 16U])
                : PRISM_DRIVER_SERVICE_NONE;

        if (init_offset >= code_size || poll_offset >= code_size
            || shutdown_offset >= code_size
            || (service_offset != PRISM_DRIVER_SERVICE_NONE
                && service_offset >= code_size)) {
            driver_runtime_destroy((uint32_t)runtime_handle);
            return -1;
        }
        for (uint32_t index = 0U; index < package.image_size; ++index) {
            driver->image[index] = package.image[index];
        }
        driver->image_size = package.image_size;
        driver->init_offset = init_offset;
        driver->poll_offset = poll_offset;
        driver->shutdown_offset = shutdown_offset;
        driver->service_offset = service_offset;
        driver->runtime_handle = (uint32_t)runtime_handle;
        driver->state = RESIDENT_DRIVER_INITIALIZING;
        driver->callback_active = 0U;
    }

    *out_id = slot + 1U;
    return 0;
}

int app_manager_driver_service_call(uint32_t id, int32_t operation,
    int32_t argument0, int32_t argument1)
{
    resident_driver_t* driver;
    int status;
    int resume = 0;
    uint32_t slices = 0U;

    if (id == 0U || id > RESIDENT_DRIVER_MAX) {
        return -1;
    }
    driver = &resident_drivers[id - 1U];
    if (driver->state != RESIDENT_DRIVER_RUNNING
        || driver->service_offset == PRISM_DRIVER_SERVICE_NONE) {
        return -1;
    }

    do {
        status = bytecode_vm_driver_service(driver->image, driver->image_size,
            driver->service_offset, driver->runtime_handle, operation,
            argument0, argument1, resume);
        if (status != BCVM_DRIVER_SLICE_YIELDED) {
            break;
        }
        resume = 1;
        ++slices;
    } while (slices < 256U);

    if (status != 0) {
        ERROR_LOG("resident driver service callback failed");
        bytecode_vm_driver_reset(driver->runtime_handle);
        driver->callback_active = 0U;
        driver->state = RESIDENT_DRIVER_STOPPING;
        return -1;
    }

    return bytecode_vm_driver_result(driver->runtime_handle);
}

int app_manager_driver_service_find(int32_t service_type)
{
    for (uint32_t slot = 0U; slot < RESIDENT_DRIVER_MAX; ++slot) {
        const resident_driver_t* driver = &resident_drivers[slot];
        int result;

        if (driver->state != RESIDENT_DRIVER_RUNNING
            || driver->service_offset == PRISM_DRIVER_SERVICE_NONE) {
            continue;
        }
        result = app_manager_driver_service_call(slot + 1U, 0,
            service_type, 0);
        if (result == 1) {
            return (int)(slot + 1U);
        }
    }
    return -1;
}

int app_manager_stop_driver(uint32_t id) {
    resident_driver_t* driver;

    if (id == 0U || id > RESIDENT_DRIVER_MAX) {
        return -1;
    }
    driver = &resident_drivers[id - 1U];
    if (driver->state == RESIDENT_DRIVER_FREE
        || driver->state == RESIDENT_DRIVER_STOPPING) {
        return -1;
    }

    bytecode_vm_driver_reset(driver->runtime_handle);
    driver->callback_active = 0U;
    driver->state = RESIDENT_DRIVER_STOPPING;
    return 0;
}

void app_manager_list_drivers(void) {
    for (uint32_t slot = 0U; slot < RESIDENT_DRIVER_MAX; ++slot) {
        const resident_driver_t* driver = &resident_drivers[slot];
        const char* state;

        if (driver->state == RESIDENT_DRIVER_FREE) {
            continue;
        }
        state = driver->state == RESIDENT_DRIVER_INITIALIZING ? "initializing"
            : driver->state == RESIDENT_DRIVER_RUNNING ? "running" : "stopping";
        console_write_uint(slot + 1U);
        console_write(": ");
        console_writeln(state);
    }
}

void app_manager_driver_tick(void) {
    uint32_t tick_ms = system_uptime_ms();

    if (tick_ms == resident_driver_last_tick_ms) {
        return;
    }
    resident_driver_last_tick_ms = tick_ms;

    for (uint32_t checked = 0U; checked < RESIDENT_DRIVER_MAX; ++checked) {
        uint32_t slot = resident_driver_cursor;
        resident_driver_t* driver = &resident_drivers[slot];
        uint32_t entry_offset;
        int result;

        resident_driver_cursor = (slot + 1U) % RESIDENT_DRIVER_MAX;
        if (driver->state == RESIDENT_DRIVER_FREE) {
            continue;
        }
        if (driver->state == RESIDENT_DRIVER_INITIALIZING) {
            entry_offset = driver->init_offset;
            DEBUG_LOG("resident driver invoking init callback");
        } else if (driver->state == RESIDENT_DRIVER_STOPPING) {
            entry_offset = driver->shutdown_offset;
            DEBUG_LOG("resident driver invoking shutdown callback");
        } else {
            entry_offset = driver->poll_offset;
        }

        result = bytecode_vm_driver_slice(driver->image, driver->image_size,
            entry_offset, driver->runtime_handle, driver->callback_active != 0U);
        if (driver->state != RESIDENT_DRIVER_STOPPING
            && bytecode_vm_driver_stop_requested(driver->runtime_handle)) {
            DEBUG_LOG("resident driver stop requested; scheduling shutdown callback");
            bytecode_vm_driver_reset(driver->runtime_handle);
            driver->callback_active = 0U;
            driver->state = RESIDENT_DRIVER_STOPPING;
            return;
        }
        if (result == BCVM_DRIVER_SLICE_YIELDED) {
            driver->callback_active = 1U;
            return;
        }
        if (result != 0) {
            DEBUG_LOG("resident driver callback failed; releasing runtime");
            console_write("Driver ");
            console_write_uint(slot + 1U);
            console_writeln(" stopped after a callback error");
            resident_driver_release(driver);
            return;
        }

        driver->callback_active = 0U;
        if (driver->state == RESIDENT_DRIVER_INITIALIZING) {
            if (bytecode_vm_driver_result(driver->runtime_handle) != 0) {
                console_write("Driver ");
                console_write_uint(slot + 1U);
                console_writeln(" initialization failed; running shutdown");
                driver->state = RESIDENT_DRIVER_STOPPING;
            } else {
                DEBUG_LOG("resident driver initialization completed");
                driver->state = RESIDENT_DRIVER_RUNNING;
            }
        } else if (driver->state == RESIDENT_DRIVER_STOPPING) {
            resident_driver_release(driver);
        }
        return;
    }
}

int app_manager_foreground_app_active(void) {
    return foreground_app_active;
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
