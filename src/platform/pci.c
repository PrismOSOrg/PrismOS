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
#define PCI_MAX_DEVICE_CLAIMS 32U

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    pci_device_owner_t owner;
} PciDeviceClaim;

static PciDeviceClaim pci_claims[PCI_MAX_DEVICE_CLAIMS];

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

uint16_t pci_config_read16(const pci_device_t* device, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(device, offset));
    return inw((uint16_t)(PCI_CONFIG_DATA + (offset & 2U)));
}

void pci_config_write16(const pci_device_t* device, uint8_t offset, uint16_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_config_address(device, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2U)), value);
}

int pci_device_is_claimed_by(const pci_device_t* device, pci_device_owner_t owner)
{
    if (device == 0 || owner == PCI_DEVICE_OWNER_NONE) {
        return 0;
    }

    for (uint32_t index = 0U; index < PCI_MAX_DEVICE_CLAIMS; ++index) {
        if (pci_claims[index].owner == owner
            && pci_claims[index].bus == device->bus
            && pci_claims[index].device == device->device
            && pci_claims[index].function == device->function) {
            return 1;
        }
    }
    return 0;
}

int pci_claim_device(const pci_device_t* device, pci_device_owner_t owner)
{
    int free_slot = -1;

    if (device == 0 || owner == PCI_DEVICE_OWNER_NONE
        || (owner != PCI_DEVICE_OWNER_KERNEL && owner != PCI_DEVICE_OWNER_PRISMCC)
        || (uint16_t)pci_config_read32(device, 0U) == PCI_VENDOR_NONE) {
        return -1;
    }

    for (uint32_t index = 0U; index < PCI_MAX_DEVICE_CLAIMS; ++index) {
        if (pci_claims[index].owner == PCI_DEVICE_OWNER_NONE) {
            if (free_slot < 0) {
                free_slot = (int)index;
            }
            continue;
        }
        if (pci_claims[index].bus == device->bus
            && pci_claims[index].device == device->device
            && pci_claims[index].function == device->function) {
            return -1;
        }
    }

    if (free_slot < 0) {
        return -1;
    }

    pci_claims[free_slot].bus = device->bus;
    pci_claims[free_slot].device = device->device;
    pci_claims[free_slot].function = device->function;
    pci_claims[free_slot].owner = owner;
    return 0;
}

void pci_release_device(const pci_device_t* device, pci_device_owner_t owner)
{
    if (device == 0 || owner == PCI_DEVICE_OWNER_NONE) {
        return;
    }

    for (uint32_t index = 0U; index < PCI_MAX_DEVICE_CLAIMS; ++index) {
        if (pci_claims[index].owner == owner
            && pci_claims[index].bus == device->bus
            && pci_claims[index].device == device->device
            && pci_claims[index].function == device->function) {
            pci_claims[index].owner = PCI_DEVICE_OWNER_NONE;
            return;
        }
    }
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
    int* out_is_io)
{
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

static uint32_t pci_irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void pci_irq_restore(uint32_t flags)
{
    if ((flags & 0x200U) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

int pci_read_bar_info(const pci_device_t* device, uint8_t bar_index,
    uint64_t* out_base, uint64_t* out_size, int* out_is_io)
{
    uint8_t header_type;
    uint8_t bar_count;
    uint8_t offset;
    uint32_t original_low;
    uint32_t original_high = 0U;
    uint32_t mask_low;
    uint32_t mask_high = 0U;
    uint16_t command;
    uint32_t flags;
    uint64_t base;
    uint64_t mask;
    uint64_t size;
    int is_io;
    int is_64_bit = 0;

    if (device == 0 || out_base == 0 || out_size == 0 || out_is_io == 0
        || bar_index >= 6U
        || (!pci_device_is_claimed_by(device, PCI_DEVICE_OWNER_KERNEL)
            && !pci_device_is_claimed_by(device, PCI_DEVICE_OWNER_PRISMCC))) {
        return -1;
    }

    header_type = (uint8_t)(device->header_type & PCI_HEADER_TYPE_MASK);
    bar_count = header_type == 0U ? 6U : (header_type == 1U ? 2U : 0U);
    if (bar_index >= bar_count) {
        return -1;
    }

    for (uint8_t previous = 0U; previous < bar_index; ++previous) {
        uint32_t previous_bar = pci_config_read32(device,
            (uint8_t)(0x10U + previous * 4U));
        if ((previous_bar & PCI_BAR_IO) == 0U
            && (previous_bar & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_64) {
            previous++;
            if (previous == bar_index) {
                return -1;
            }
        }
    }

    offset = (uint8_t)(0x10U + bar_index * 4U);
    original_low = pci_config_read32(device, offset);
    if (original_low == 0U || original_low == 0xFFFFFFFFU) {
        return -1;
    }

    is_io = (original_low & PCI_BAR_IO) != 0U;
    if (is_io) {
        base = original_low & 0xFFFFFFFCU;
    } else {
        uint32_t memory_type = original_low & PCI_BAR_MEMORY_TYPE_MASK;
        if (memory_type != 0U && memory_type != PCI_BAR_MEMORY_64) {
            return -1;
        }
        is_64_bit = memory_type == PCI_BAR_MEMORY_64;
        if (is_64_bit) {
            if (bar_index + 1U >= bar_count) {
                return -1;
            }
            original_high = pci_config_read32(device, (uint8_t)(offset + 4U));
        }
        base = (uint64_t)(original_low & 0xFFFFFFF0U)
            | ((uint64_t)original_high << 32);
    }

    flags = pci_irq_save();
    command = pci_config_read16(device, 0x04U);
    pci_config_write16(device, 0x04U, (uint16_t)(command & (uint16_t)~0x0007U));
    pci_config_write32(device, offset, 0xFFFFFFFFU);
    if (is_64_bit) {
        pci_config_write32(device, (uint8_t)(offset + 4U), 0xFFFFFFFFU);
    }
    mask_low = pci_config_read32(device, offset);
    if (is_64_bit) {
        mask_high = pci_config_read32(device, (uint8_t)(offset + 4U));
    }
    pci_config_write32(device, offset, original_low);
    if (is_64_bit) {
        pci_config_write32(device, (uint8_t)(offset + 4U), original_high);
    }
    pci_config_write16(device, 0x04U, command);
    pci_irq_restore(flags);

    if (is_io) {
        uint32_t mask32 = mask_low & 0xFFFFFFFCU;
        size = (uint32_t)(~mask32 + 1U);
        if (size == 0U || base + size > 0x10000U) {
            return -1;
        }
    } else {
        if (is_64_bit) {
            mask = ((uint64_t)mask_high << 32) | (mask_low & 0xFFFFFFF0U);
            size = ~mask + 1ULL;
        } else {
            uint32_t mask32 = mask_low & 0xFFFFFFF0U;
            size = (uint32_t)(~mask32 + 1U);
        }
        if (size == 0U || base + size < base) {
            return -1;
        }
    }

    if ((size & (size - 1U)) != 0U) {
        return -1;
    }

    *out_base = base;
    *out_size = size;
    *out_is_io = is_io;
    return 0;
}
