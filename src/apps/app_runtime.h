#ifndef PRISMOS_APPS_APP_RUNTIME_H
#define PRISMOS_APPS_APP_RUNTIME_H

#include <stdint.h>

int app_runtime_run(const uint8_t* image, uint32_t image_size, const char* args);
int app_runtime_run_driver(const uint8_t* image, uint32_t image_size, const char* args);

#endif
