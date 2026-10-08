#include "platform/pci.h"

#include "platform/io.h"

#define PCI_CONFIG_ADDRESS 0x0CF8U
#define PCI_CONFIG_DATA 0x0CFcU
#define PCI_CONFIG_ENABLE 0x80000000U
#define PCI_VENDOR_NONE 0xFFFFU
#define PCI_HEADER_MULTIFUNCTION 0x80U
#define PCI_HEADER_TYPE_MASK 0x7FU
#define PCI_BAR_IO 0x1U
#define PCI_BAR_MEMORY_TYPE_MASK 0x6U
#define PCI_BAR_MEMORY_64 0x4U

static uint32_t pci_config_address(const pci_device_t* device, uint8_t offset) {
    return PCI_CONFIG_ENABLE
        | ((uint32_t)device->bus << 16)
        | ((uint32_t)device->device << 11)
        | ((uint32_t)device->function << 8)
        | ((uint32_t)offset & 0xFCU);
}

uint32_t pci_config_read32(const pci_device_t* device, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(device, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(const pci_device_t* device, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(device, offset));
    outl(PCI_CONFIG_DATA, value);
}

static int pci_read_function(uint8_t bus, uint8_t device_number, uint8_t function,
    pci_device_t* out_device) {
    pci_device_t device = {0};
    uint32_t identity;
    uint32_t class_data;
    uint32_t interrupt_data;

    device.bus = bus;
    device.device = device_number;
    device.function = function;
    identity = pci_config_read32(&device, 0x00U);
    if ((uint16_t)identity == PCI_VENDOR_NONE) {
        return 0;
    }

    class_data = pci_config_read32(&device, 0x08U);
    interrupt_data = pci_config_read32(&device, 0x3CU);
    device.vendor_id = (uint16_t)(identity & 0xFFFFU);
    device.device_id = (uint16_t)(identity >> 16);
    device.class_code = (uint8_t)(class_data >> 24);
    device.subclass = (uint8_t)(class_data >> 16);
    device.programming_interface = (uint8_t)(class_data >> 8);
    device.interrupt_line = (uint8_t)(interrupt_data & 0xFFU);
    device.header_type = (uint8_t)(pci_config_read32(&device, 0x0CU) >> 16);
    *out_device = device;
    return 1;
}

int pci_scan(pci_scan_visitor_t visitor, void* context) {
    int found = 0;

    if (visitor == 0) {
        return -1;
    }

    for (uint32_t bus = 0U; bus < 256U; bus++) {
        for (uint32_t device_number = 0U; device_number < 32U; device_number++) {
            pci_device_t device;
            if (!pci_read_function((uint8_t)bus, (uint8_t)device_number, 0U, &device)) {
                continue;
            }
            found++;
            if (visitor(&device, context) != 0) {
                return found;
            }

            if ((device.header_type & PCI_HEADER_MULTIFUNCTION) == 0U) {
                continue;
            }
            for (uint32_t function = 1U; function < 8U; function++) {
                if (pci_read_function((uint8_t)bus, (uint8_t)device_number,
                    (uint8_t)function, &device)) {
                    found++;
                    if (visitor(&device, context) != 0) {
                        return found;
                    }
                }
            }
        }
    }

    return found;
}

int pci_read_bar(const pci_device_t* device, uint8_t bar_index, uint64_t* out_base,
    int* out_is_io) {
    uint8_t header_type;
    uint8_t bar_count;
    uint32_t bar;

    if (device == 0 || out_base == 0 || out_is_io == 0 || bar_index >= 6U) {
        return -1;
    }
    header_type = (uint8_t)(device->header_type & PCI_HEADER_TYPE_MASK);
    bar_count = header_type == 0U ? 6U : (header_type == 1U ? 2U : 0U);
    if (bar_index >= bar_count) {
        return -1;
    }

    bar = pci_config_read32(device, (uint8_t)(0x10U + bar_index * 4U));
    if (bar == 0U || bar == 0xFFFFFFFFU) {
        return -1;
    }
    if ((bar & PCI_BAR_IO) != 0U) {
        *out_base = (uint64_t)(bar & 0xFFFFFFFCU);
        *out_is_io = 1;
        return 0;
    }

    *out_base = (uint64_t)(bar & 0xFFFFFFF0U);
    *out_is_io = 0;
    if ((bar & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_64) {
        uint32_t high;
        if (bar_index + 1U >= bar_count) {
            return -1;
        }
        high = pci_config_read32(device, (uint8_t)(0x14U + bar_index * 4U));
        *out_base |= (uint64_t)high << 32;
    }
    return 0;
}
