#ifndef PRISMOS_APPS_USER_DRIVER_VM_H
#define PRISMOS_APPS_USER_DRIVER_VM_H

#include <stdint.h>

int user_driver_vm_slice(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int resume);
int user_driver_vm_service(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int32_t operation,
    int32_t argument0, int32_t argument1, int resume);
void user_driver_vm_reset(uint32_t runtime_handle);
void user_driver_vm_destroy(uint32_t runtime_handle);
int32_t user_driver_vm_result(uint32_t runtime_handle);
int user_driver_vm_stop_requested(uint32_t runtime_handle);
int user_driver_vm_self_test(void);
int user_driver_vm_isolation_available(void);

#endif
