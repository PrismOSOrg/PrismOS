#include "net/drivers/e1000.h"

#include <stdint.h>

#include "debug/log.h"
#include "memory/paging.h"
#include "net/network.h"
#include "platform/io.h"
#include "platform/pci.h"

#define E1000_VENDOR_ID 0x8086U
#define E1000_DEVICE_ID 0x100EU
#define E1000_BAR0_SIZE 0x20000U
#define E1000_MMIO_VIRTUAL_BASE 0xE0000000U
#define E1000_REGISTER_CTRL 0x0000U
#define E1000_REGISTER_STATUS 0x0008U
#define E1000_REGISTER_IMC 0x00D8U
#define E1000_REGISTER_RCTL 0x0100U
#define E1000_REGISTER_TCTL 0x0400U
#define E1000_REGISTER_TIPG 0x0410U
#define E1000_REGISTER_RDBAL 0x2800U
#define E1000_REGISTER_RDBAH 0x2804U
#define E1000_REGISTER_RDLEN 0x2808U
#define E1000_REGISTER_RDH 0x2810U
#define E1000_REGISTER_RDT 0x2818U
#define E1000_REGISTER_TDBAL 0x3800U
#define E1000_REGISTER_TDBAH 0x3804U
#define E1000_REGISTER_TDLEN 0x3808U
#define E1000_REGISTER_TDH 0x3810U
#define E1000_REGISTER_TDT 0x3818U
#define E1000_REGISTER_RAL0 0x5400U
#define E1000_REGISTER_RAH0 0x5404U
#define E1000_CTRL_RESET 0x04000000U
#define E1000_CTRL_SET_LINK_UP 0x00000040U
#define E1000_STATUS_LINK_UP 0x00000002U
#define E1000_RCTL_ENABLE 0x00000002U
#define E1000_RCTL_BROADCAST_ACCEPT 0x00008000U
#define E1000_RCTL_STRIP_CRC 0x04000000U
#define E1000_TCTL_ENABLE 0x00000002U
#define E1000_TCTL_PAD_SHORT_PACKETS 0x00000008U
#define E1000_TX_STATUS_DONE 0x01U
#define E1000_RX_STATUS_DONE 0x01U
#define E1000_RX_STATUS_END_PACKET 0x02U
#define E1000_TX_COMMAND_END_PACKET 0x01U
#define E1000_TX_COMMAND_INSERT_FCS 0x02U
#define E1000_TX_COMMAND_REPORT_STATUS 0x08U
#define E1000_DESCRIPTOR_COUNT 16U
#define E1000_RX_BUFFER_SIZE 2048U
#define E1000_WAIT_LIMIT 1000000U
#define PCI_COMMAND_REGISTER 0x04U
#define PCI_COMMAND_MEMORY_SPACE 0x00000002U
#define PCI_COMMAND_BUS_MASTER 0x00000004U
#define PCI_COMMAND_INTERRUPT_DISABLE 0x00000400U
#define E1000_MMIO_FLAGS (PAGING_FLAG_READ_WRITE | PAGING_FLAG_WRITE_THROUGH | PAGING_FLAG_CACHE_DISABLE)

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t address_low;
    uint32_t address_high;
    volatile uint16_t length;
    uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    uint16_t special;
} E1000ReceiveDescriptor;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t address_low;
    uint32_t address_high;
    volatile uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    volatile uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} E1000TransmitDescriptor;

typedef struct {
    volatile uint32_t* registers;
    network_interface_t* interface;
    uint32_t receive_head;
    uint32_t transmit_tail;
} E1000State;

static E1000State e1000;
static E1000ReceiveDescriptor receive_descriptors[E1000_DESCRIPTOR_COUNT]
    __attribute__((aligned(16)));
static E1000TransmitDescriptor transmit_descriptors[E1000_DESCRIPTOR_COUNT]
    __attribute__((aligned(16)));
static uint8_t receive_buffers[E1000_DESCRIPTOR_COUNT][E1000_RX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static uint8_t transmit_buffers[E1000_DESCRIPTOR_COUNT][NETWORK_MAX_FRAME_SIZE]
    __attribute__((aligned(16)));

static uint32_t e1000_read(uint32_t offset) {
    return e1000.registers[offset / sizeof(uint32_t)];
}

static void e1000_write(uint32_t offset, uint32_t value) {
    e1000.registers[offset / sizeof(uint32_t)] = value;
}

static void e1000_delay(void) {
    for (uint32_t index = 0U; index < 1000U; index++) {
        (void)e1000_read(E1000_REGISTER_STATUS);
    }
}

static void e1000_set_descriptor_address(uint32_t* low, uint32_t* high,
    const void* address) {
    uint64_t physical = (uint64_t)(uintptr_t)address;
    *low = (uint32_t)physical;
    *high = (uint32_t)(physical >> 32);
}

static void e1000_set_ring(uint32_t low_register, uint32_t high_register,
    uint32_t length_register, uint32_t head_register, uint32_t tail_register,
    void* descriptors, uint32_t descriptor_count) {
    uint64_t physical = (uint64_t)(uintptr_t)descriptors;
    e1000_write(low_register, (uint32_t)physical);
    e1000_write(high_register, (uint32_t)(physical >> 32));
    e1000_write(length_register, descriptor_count * 16U);
    e1000_write(head_register, 0U);
    e1000_write(tail_register, descriptor_count - 1U);
}

static int e1000_transmit(void* context, const uint8_t* frame, uint16_t length) {
    E1000State* state = (E1000State*)context;
    uint32_t index = state->transmit_tail;
    E1000TransmitDescriptor* descriptor = &transmit_descriptors[index];

    for (uint32_t guard = 0U; (descriptor->status & E1000_TX_STATUS_DONE) == 0U
        && guard < E1000_WAIT_LIMIT; guard++) {
        cpu_relax();
    }
    if ((descriptor->status & E1000_TX_STATUS_DONE) == 0U) {
        return -1;
    }

    for (uint32_t byte = 0U; byte < length; byte++) {
        transmit_buffers[index][byte] = frame[byte];
    }
    e1000_set_descriptor_address(&descriptor->address_low, &descriptor->address_high,
        transmit_buffers[index]);
    descriptor->length = length;
    descriptor->checksum_offset = 0U;
    descriptor->command = E1000_TX_COMMAND_END_PACKET
        | E1000_TX_COMMAND_INSERT_FCS | E1000_TX_COMMAND_REPORT_STATUS;
    descriptor->status = 0U;
    descriptor->checksum_start = 0U;
    descriptor->special = 0U;

    state->transmit_tail = (index + 1U) % E1000_DESCRIPTOR_COUNT;
    e1000_write(E1000_REGISTER_TDT, state->transmit_tail);
    return 0;
}

static void e1000_poll(void* context) {
    E1000State* state = (E1000State*)context;
    state->interface->link_up = (uint8_t)((e1000_read(E1000_REGISTER_STATUS)
        & E1000_STATUS_LINK_UP) != 0U);

    for (uint32_t processed = 0U; processed < E1000_DESCRIPTOR_COUNT; processed++) {
        uint32_t index = state->receive_head;
        E1000ReceiveDescriptor* descriptor = &receive_descriptors[index];
        uint8_t status = descriptor->status;

        if ((status & E1000_RX_STATUS_DONE) == 0U) {
            break;
        }
        if ((status & E1000_RX_STATUS_END_PACKET) != 0U
            && descriptor->errors == 0U && descriptor->length <= E1000_RX_BUFFER_SIZE) {
            (void)network_submit_received_frame(state->interface, receive_buffers[index],
                descriptor->length);
        } else {
            state->interface->stats.receive_errors++;
            DEBUG_LOG("e1000: rejected RX descriptor because packet status or errors were invalid");
        }

        descriptor->status = 0U;
        descriptor->errors = 0U;
        state->receive_head = (index + 1U) % E1000_DESCRIPTOR_COUNT;
        e1000_write(E1000_REGISTER_RDT, index);
    }
}

static const network_driver_ops_t e1000_operations = {
    e1000_transmit,
    e1000_poll,
};

static int e1000_find_pci_device(const pci_device_t* device, void* context) {
    pci_device_t* result = (pci_device_t*)context;
    if (device->vendor_id == E1000_VENDOR_ID && device->device_id == E1000_DEVICE_ID
        && device->class_code == 0x02U) {
        *result = *device;
        return 1;
    }
    return 0;
}

static int e1000_map_registers(uint64_t physical_base) {
    if (physical_base == 0U || physical_base > 0xFFFFFFFFULL
        || physical_base + E1000_BAR0_SIZE > 0x100000000ULL) {
        return -1;
    }
    for (uint32_t offset = 0U; offset < E1000_BAR0_SIZE; offset += 4096U) {
        paging_map(E1000_MMIO_VIRTUAL_BASE + offset,
            (uint32_t)physical_base + offset, E1000_MMIO_FLAGS);
    }
    e1000.registers = (volatile uint32_t*)(uintptr_t)E1000_MMIO_VIRTUAL_BASE;
    return 0;
}

static int e1000_initialize_device(const pci_device_t* device) {
    uint64_t bar_base;
    int bar_is_io;
    uint32_t command;
    uint32_t control;
    uint32_t mac_low;
    uint32_t mac_high;
    uint8_t mac_address[6];

    if (pci_read_bar(device, 0U, &bar_base, &bar_is_io) != 0 || bar_is_io
        || e1000_map_registers(bar_base) != 0) {
        return -1;
    }

    command = pci_config_read32(device, PCI_COMMAND_REGISTER);
    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INTERRUPT_DISABLE;
    pci_config_write32(device, PCI_COMMAND_REGISTER, command);

    control = e1000_read(E1000_REGISTER_CTRL);
    e1000_write(E1000_REGISTER_CTRL, control | E1000_CTRL_RESET);
    for (uint32_t guard = 0U; guard < E1000_WAIT_LIMIT; guard++) {
        if ((e1000_read(E1000_REGISTER_CTRL) & E1000_CTRL_RESET) == 0U) {
            break;
        }
        cpu_relax();
    }
    e1000_delay();
    e1000_write(E1000_REGISTER_IMC, 0xFFFFFFFFU);

    mac_low = e1000_read(E1000_REGISTER_RAL0);
    mac_high = e1000_read(E1000_REGISTER_RAH0);
    if ((mac_high & 0x80000000U) == 0U) {
        ERROR_LOG("e1000: permanent MAC address unavailable");
        return -1;
    }
    mac_address[0] = (uint8_t)mac_low;
    mac_address[1] = (uint8_t)(mac_low >> 8);
    mac_address[2] = (uint8_t)(mac_low >> 16);
    mac_address[3] = (uint8_t)(mac_low >> 24);
    mac_address[4] = (uint8_t)mac_high;
    mac_address[5] = (uint8_t)(mac_high >> 8);

    for (uint32_t index = 0U; index < E1000_DESCRIPTOR_COUNT; index++) {
        E1000ReceiveDescriptor* rx = &receive_descriptors[index];
        E1000TransmitDescriptor* tx = &transmit_descriptors[index];
        e1000_set_descriptor_address(&rx->address_low, &rx->address_high, receive_buffers[index]);
        rx->length = 0U;
        rx->checksum = 0U;
        rx->status = 0U;
        rx->errors = 0U;
        rx->special = 0U;
        e1000_set_descriptor_address(&tx->address_low, &tx->address_high, transmit_buffers[index]);
        tx->length = 0U;
        tx->checksum_offset = 0U;
        tx->command = 0U;
        tx->status = E1000_TX_STATUS_DONE;
        tx->checksum_start = 0U;
        tx->special = 0U;
    }

    e1000_set_ring(E1000_REGISTER_RDBAL, E1000_REGISTER_RDBAH,
        E1000_REGISTER_RDLEN, E1000_REGISTER_RDH, E1000_REGISTER_RDT,
        receive_descriptors, E1000_DESCRIPTOR_COUNT);
    e1000_set_ring(E1000_REGISTER_TDBAL, E1000_REGISTER_TDBAH,
        E1000_REGISTER_TDLEN, E1000_REGISTER_TDH, E1000_REGISTER_TDT,
        transmit_descriptors, E1000_DESCRIPTOR_COUNT);
    e1000_write(E1000_REGISTER_TDT, 0U);
    e1000_write(E1000_REGISTER_TIPG, 0x0060200AU);
    e1000_write(E1000_REGISTER_TCTL, 0x0004010AU
        | E1000_TCTL_ENABLE | E1000_TCTL_PAD_SHORT_PACKETS);
    e1000_write(E1000_REGISTER_RCTL, E1000_RCTL_ENABLE
        | E1000_RCTL_BROADCAST_ACCEPT | E1000_RCTL_STRIP_CRC);
    e1000_write(E1000_REGISTER_CTRL, e1000_read(E1000_REGISTER_CTRL) | E1000_CTRL_SET_LINK_UP);
    e1000_delay();

    e1000.receive_head = 0U;
    e1000.transmit_tail = 0U;
    e1000.interface = network_register_interface("e1000", mac_address, 1500U,
        (uint8_t)((e1000_read(E1000_REGISTER_STATUS) & E1000_STATUS_LINK_UP) != 0U),
        &e1000_operations, &e1000);
    if (e1000.interface == 0) {
        return -1;
    }

    DEBUG_LOG("e1000: polling Ethernet interface initialized");
    return 0;
}

int e1000_driver_init(void) {
    pci_device_t device = {0};

    if (pci_scan(e1000_find_pci_device, &device) <= 0
        || device.vendor_id != E1000_VENDOR_ID) {
        return -1;
    }
    return e1000_initialize_device(&device);
}
