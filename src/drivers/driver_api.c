#include "drivers/driver_api.h"

#include "comport/comport.h"
#include "platform/io.h"

#define DRIVER_MMIO_IDENTITY_LIMIT 0x04000000U

typedef struct {
    uint32_t base;
    uint32_t size;
} DriverMmioRegion;

static DriverMmioRegion mmio_regions[DRIVER_MMIO_MAX_REGIONS];
static uint8_t mmio_region_count;
static int driver_active;

int driver_runtime_begin(void) {
    if (driver_active) {
        return -1;
    }

    if (comport_init() != 0) {
        return -1;
    }

    driver_active = 1;
    return 0;
}

void driver_runtime_end(void) {
    if (driver_active) {
        /* Recover the kernel COM1 receive interrupt even if VM execution failed. */
        outb(DRIVER_COM1_BASE + 1U, 0x01U);
    }
    driver_active = 0;
}

int driver_runtime_is_active(void) {
    return driver_active;
}

int driver_api_serial_write(const char* bytes, uint32_t length) {
    if (!driver_active || bytes == 0 || length > 4096U) {
        return -1;
    }

    for (uint32_t index = 0; index < length; index++) {
        comport_write_char(bytes[index]);
    }

    return 0;
}

int driver_api_serial_read(void) {
    if (!driver_active) {
        return -1;
    }

    return comport_poll_char();
}

int driver_api_port_read8(uint32_t port) {
    if (!driver_active || port < DRIVER_COM1_BASE || port >= DRIVER_COM1_BASE + DRIVER_COM1_PORT_COUNT) {
        return -1;
    }

    return (int)inb((uint16_t)port);
}

int driver_api_port_write8(uint32_t port, uint32_t value) {
    if (!driver_active
        || port < DRIVER_COM1_BASE
        || port >= DRIVER_COM1_BASE + DRIVER_COM1_PORT_COUNT
        || value > 0xFFU) {
        return -1;
    }

    outb((uint16_t)port, (uint8_t)value);
    return 0;
}

int driver_mmio_register_region(uint32_t base, uint32_t size) {
    if (size == 0U
        || base >= DRIVER_MMIO_IDENTITY_LIMIT
        || size > DRIVER_MMIO_IDENTITY_LIMIT - base
        || mmio_region_count >= DRIVER_MMIO_MAX_REGIONS) {
        return -1;
    }

    for (uint32_t index = 0; index < mmio_region_count; index++) {
        uint32_t region_end = mmio_regions[index].base + mmio_regions[index].size;
        uint32_t request_end = base + size;
        if (base < region_end && mmio_regions[index].base < request_end) {
            return -1;
        }
    }

    mmio_regions[mmio_region_count].base = base;
    mmio_regions[mmio_region_count].size = size;
    mmio_region_count++;
    return 0;
}

void driver_mmio_clear_regions(void) {
    mmio_region_count = 0U;
}

static int driver_mmio_address_allowed(uint32_t address) {
    if ((address & 3U) != 0U || address > DRIVER_MMIO_IDENTITY_LIMIT - sizeof(uint32_t)) {
        return 0;
    }

    for (uint32_t index = 0; index < mmio_region_count; index++) {
        uint32_t offset = address - mmio_regions[index].base;
        if (address >= mmio_regions[index].base
            && offset <= mmio_regions[index].size
            && sizeof(uint32_t) <= mmio_regions[index].size - offset) {
            return 1;
        }
    }

    return 0;
}

int driver_api_mmio_read32(uint32_t address) {
    if (!driver_active || !driver_mmio_address_allowed(address)) {
        return -1;
    }

    return (int)*(volatile uint32_t*)(uintptr_t)address;
}

int driver_api_mmio_write32(uint32_t address, uint32_t value) {
    if (!driver_active || !driver_mmio_address_allowed(address)) {
        return -1;
    }

    *(volatile uint32_t*)(uintptr_t)address = value;
    return 0;
}
