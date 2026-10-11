#ifndef PRISMOS_APPS_APP_MANAGER_H
#define PRISMOS_APPS_APP_MANAGER_H

#include <stdint.h>

#include "apps/app_format.h"

int app_manager_init(void);
int app_manager_is_ready(void);
int app_manager_probe_path(const char* abs_path, prism_app_header_t* out_header);
int app_manager_run_path(const char* app_abs_path, const char* args);
int app_manager_run_driver_path(const char* driver_abs_path);
int app_manager_start_driver_path(const char* driver_abs_path, uint32_t* out_id);
int app_manager_driver_service_call(uint32_t id, int32_t operation,
    int32_t argument0, int32_t argument1);
int app_manager_driver_service_find(int32_t service_type);
int app_manager_stop_driver(uint32_t id);
void app_manager_list_drivers(void);
void app_manager_driver_tick(void);
int app_manager_foreground_app_active(void);
int app_manager_run_editor(const char* target_abs_path);
int app_manager_run_ide(const char* target_abs_path);

#endif
