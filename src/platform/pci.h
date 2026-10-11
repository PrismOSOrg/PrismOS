#ifndef PRISMOS_PLATFORM_PCI_H
#define PRISMOS_PLATFORM_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t interrupt_line;
    uint8_t header_type;
} pci_device_t;

typedef enum {
    PCI_DEVICE_OWNER_NONE = 0,
    PCI_DEVICE_OWNER_KERNEL = 1,
    PCI_DEVICE_OWNER_PRISMCC = 2,
} pci_device_owner_t;

typedef int (*pci_scan_visitor_t)(const pci_device_t* device, void* context);

int pci_scan(pci_scan_visitor_t visitor, void* context);
uint32_t pci_config_read32(const pci_device_t* device, uint8_t offset);
void pci_config_write32(const pci_device_t* device, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(const pci_device_t* device, uint8_t offset);
void pci_config_write16(const pci_device_t* device, uint8_t offset, uint16_t value);
int pci_read_bar(const pci_device_t* device, uint8_t bar_index, uint64_t* out_base, int* out_is_io);
int pci_read_bar_info(const pci_device_t* device, uint8_t bar_index,
    uint64_t* out_base, uint64_t* out_size, int* out_is_io);
int pci_claim_device(const pci_device_t* device, pci_device_owner_t owner);
void pci_release_device(const pci_device_t* device, pci_device_owner_t owner);
int pci_device_is_claimed_by(const pci_device_t* device, pci_device_owner_t owner);

#endif
