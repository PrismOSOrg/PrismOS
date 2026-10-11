#ifndef PRISMOS_APPS_BYTECODE_VM_H
#define PRISMOS_APPS_BYTECODE_VM_H

#include <stdint.h>

#define BCVM_DRIVER_SLICE_YIELDED 1
#define BCVM_DRIVER_SLICE_FAILED (-1)

int bytecode_vm_run(const uint8_t* image, uint32_t image_size, const char* args);
int bytecode_vm_run_driver(const uint8_t* image, uint32_t image_size, const char* args);
int bytecode_vm_driver_slice(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int resume);
int bytecode_vm_driver_service(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int32_t operation,
    int32_t argument0, int32_t argument1, int resume);
int bytecode_vm_driver_service_find(int32_t service_type);
void bytecode_vm_driver_reset(uint32_t runtime_handle);
void bytecode_vm_driver_destroy(uint32_t runtime_handle);
int32_t bytecode_vm_driver_result(uint32_t runtime_handle);
int bytecode_vm_driver_stop_requested(uint32_t runtime_handle);
int bytecode_vm_driver_isolation_available(void);

#endif
