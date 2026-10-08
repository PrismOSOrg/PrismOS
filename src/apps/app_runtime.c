#include "apps/app_runtime.h"

#include "apps/app_format.h"
#include "apps/bank_app.h"
#include "apps/bytecode_vm.h"
#include "apps/editor_app.h"
#include "apps/ide_app.h"
#include "debug/log.h"

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

int app_runtime_run(const uint8_t* image, uint32_t image_size, const char* args) {
    char app_id[16];
    uint32_t copy_size;

    if (image == 0 || image_size == 0U) {
        ERROR_LOG("app runtime received empty image");
        return -1;
    }

    if (image_size >= 4U
        && image[0] == 'B'
        && image[1] == 'C'
        && image[2] == 'V'
        && image[3] == 'M') {
        DEBUG_LOG("app runtime dispatching BCVM app");
        return bytecode_vm_run(image, image_size, args);
    }

    copy_size = image_size;
    if (copy_size >= sizeof(app_id)) {
        copy_size = sizeof(app_id) - 1U;
    }

    for (uint32_t i = 0; i < copy_size; i++) {
        app_id[i] = (char)image[i];
    }
    app_id[copy_size] = '\0';

    if (string_equals(app_id, "EDITOR")) {
        DEBUG_LOG("app runtime dispatching EDITOR app");
        return editor_app_run(args);
    }

    if (string_equals(app_id, "IDE")) {
        DEBUG_LOG("app runtime dispatching IDE app");
        return ide_app_run(args);
    }

    if (string_equals(app_id, "BANK")) {
        DEBUG_LOG("app runtime dispatching BANK app");
        return bank_app_run(args);
    }

    ERROR_LOG("app runtime unknown app id");
    return -1;
}

int app_runtime_run_driver(const uint8_t* image, uint32_t image_size, const char* args) {
    if (image == 0 || image_size < 4U
        || image[0] != 'B' || image[1] != 'C' || image[2] != 'V' || image[3] != 'M') {
        ERROR_LOG("driver runtime rejected non-BCVM image");
        return -1;
    }

    DEBUG_LOG("app runtime dispatching PrismCC driver");
    return bytecode_vm_run_driver(image, image_size, args == 0 ? "" : args);
}
