#ifndef PRISMOS_APPS_APP_MANAGER_H
#define PRISMOS_APPS_APP_MANAGER_H

#include <stdint.h>

#include "apps/app_format.h"

int app_manager_init(void);
int app_manager_is_ready(void);
int app_manager_probe_path(const char* abs_path, prism_app_header_t* out_header);
int app_manager_run_path(const char* app_abs_path, const char* args);
int app_manager_run_driver_path(const char* driver_abs_path);
int app_manager_run_editor(const char* target_abs_path);
int app_manager_run_ide(const char* target_abs_path);

#endif
