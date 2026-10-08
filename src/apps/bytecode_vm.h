#ifndef PRISMOS_APPS_BYTECODE_VM_H
#define PRISMOS_APPS_BYTECODE_VM_H

#include <stdint.h>

int bytecode_vm_run(const uint8_t* image, uint32_t image_size, const char* args);
int bytecode_vm_run_driver(const uint8_t* image, uint32_t image_size, const char* args);

#endif
