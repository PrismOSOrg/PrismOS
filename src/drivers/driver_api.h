#ifndef PRISMOS_DRIVERS_DRIVER_API_H
#define PRISMOS_DRIVERS_DRIVER_API_H

#include <stdint.h>

#define DRIVER_ABI_VERSION 1U
#define DRIVER_COM1_BASE 0x3F8U
#define DRIVER_COM1_PORT_COUNT 8U
#define DRIVER_MMIO_MAX_REGIONS 8U

/* Kernel-controlled lifecycle. Only one PrismCC driver may run at a time. */
int driver_runtime_begin(void);
void driver_runtime_end(void);
int driver_runtime_is_active(void);

/* PrismCC host calls. These fail with -1 outside an active driver. */
int driver_api_serial_write(const char* bytes, uint32_t length);
int driver_api_serial_read(void);
int driver_api_port_read8(uint32_t port);
int driver_api_port_write8(uint32_t port, uint32_t value);
int driver_api_mmio_read32(uint32_t address);
int driver_api_mmio_write32(uint32_t address, uint32_t value);

/* Kernel-only registration for MMIO ranges owned by a trusted device driver. */
int driver_mmio_register_region(uint32_t base, uint32_t size);
void driver_mmio_clear_regions(void);

#endif
