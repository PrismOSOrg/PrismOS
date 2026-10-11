#ifndef PRISMOS_DRIVERS_DRIVER_API_H
#define PRISMOS_DRIVERS_DRIVER_API_H

#include <stdint.h>

#define DRIVER_ABI_VERSION 1U
#define DRIVER_COM1_BASE 0x3F8U
#define DRIVER_COM1_PORT_COUNT 8U
#define DRIVER_MMIO_MAX_REGIONS 8U
#define DRIVER_IO_MAX_REGIONS 8U
#define DRIVER_RUNTIME_MAX 4U
#define DRIVER_IRQ_QUEUE_SIZE 32U
#define DRIVER_DMA_MAX_BUFFERS 8U
#define DRIVER_STATE_SLOTS 32U
#define DRIVER_DMA_MAX_SIZE 65536U
#define DRIVER_DMA_ADDRESS_LIMIT 0x100000000ULL
#define DRIVER_DMA_VIRTUAL_BASE 0xE1000000U
#define DRIVER_DMA_VIRTUAL_SIZE 0x01000000U
#define DRIVER_PCI_MMIO_MAX_SIZE 0x00800000U
#define DRIVER_PCI_MMIO_VIRTUAL_BASE 0xF0000000U
#define DRIVER_PCI_MMIO_VIRTUAL_SIZE 0x01000000U

#define DRIVER_PCI_ENABLE_IO 0x01U
#define DRIVER_PCI_ENABLE_MEMORY 0x02U
#define DRIVER_PCI_ENABLE_BUS_MASTER 0x04U

/* Kernel-controlled lifecycle. Resident drivers are time-sliced on one CPU. */
int driver_runtime_begin(void);
void driver_runtime_end(void);
int driver_runtime_is_active(void);
int driver_runtime_create(void);
int driver_runtime_activate(uint32_t handle);
void driver_runtime_deactivate(void);
void driver_runtime_destroy(uint32_t handle);
int driver_api_state_read(uint32_t slot);
int driver_api_state_write(uint32_t slot, uint32_t value);
int driver_api_irq_read(void);
int driver_api_irq_dropped(void);
int driver_api_irq_ack(void);
void driver_runtime_notify_irq(uint8_t irq);
int driver_runtime_irq_self_test(void);

/* PrismCC host calls. These fail with -1 outside an active driver. */
int driver_api_serial_write(const char* bytes, uint32_t length);
int driver_api_serial_read(void);
int driver_api_port_read8(uint32_t port);
int driver_api_port_write8(uint32_t port, uint32_t value);
int driver_api_mmio_read32(uint32_t address);
int driver_api_mmio_write32(uint32_t address, uint32_t value);
/* PCI BDF values are encoded as bus << 16 | device << 8 | function. */
int driver_api_pci_find(uint32_t vendor_id, uint32_t device_id);
int driver_api_pci_read_config32(uint32_t bdf, uint32_t offset);
int driver_api_pci_read_bar32(uint32_t bdf, uint32_t bar_index);
int driver_api_pci_claim(uint32_t bdf);
int driver_api_pci_subscribe_irq(uint32_t bdf);
int driver_api_pci_enable(uint32_t bdf, uint32_t flags);
int driver_api_pci_disable(uint32_t bdf, uint32_t flags);
int driver_api_pci_claim_io_bar(uint32_t bdf, uint32_t bar_index);
int driver_api_pci_claim_mmio_bar(uint32_t bdf, uint32_t bar_index);
int driver_api_pci_io_read(uint32_t region, uint32_t offset, uint32_t width);
int driver_api_pci_io_write(uint32_t region, uint32_t offset, uint32_t width, uint32_t value);
int driver_api_pci_mmio_read32(uint32_t region, uint32_t offset);
int driver_api_pci_mmio_write32(uint32_t region, uint32_t offset, uint32_t value);
int driver_api_dma_alloc(uint32_t size);
int driver_api_dma_alloc_below(uint32_t size, uint32_t max_address_exclusive);
int driver_api_dma_physical(uint32_t handle);
int driver_api_dma_read(uint32_t handle, uint32_t offset, uint32_t width);
int driver_api_dma_write(uint32_t handle, uint32_t offset, uint32_t width, uint32_t value);
int driver_api_dma_free(uint32_t handle);

/* Kernel-only registration for MMIO ranges owned by a trusted device driver. */
int driver_mmio_register_region(uint32_t base, uint32_t size);
void driver_mmio_clear_regions(void);

#endif
