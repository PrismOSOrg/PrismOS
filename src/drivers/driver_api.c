#include "drivers/driver_api.h"

#include "comport/comport.h"
#include "debug/log.h"
#include "memory/paging.h"
#include "memory/pmm.h"
#include "interrupts/irq.h"
#include "platform/io.h"
#include "platform/pci.h"

#define DRIVER_MMIO_IDENTITY_LIMIT 0x04000000U

typedef struct {
    uint32_t base;
    uint32_t size;
} DriverMmioRegion;

typedef struct {
    uint16_t base;
    uint32_t size;
} DriverIoRegion;

typedef struct {
    uint8_t* memory;
    uint32_t physical_base;
    uint32_t virtual_base;
    uint32_t size;
    uint32_t page_count;
} DriverDmaBuffer;

typedef struct {
    uint32_t mapped_base;
    uint32_t size;
    uint32_t page_base;
    uint32_t page_count;
    uint32_t physical_base;
} DriverPciMmioRegion;

typedef struct {
    DriverIoRegion io_regions[DRIVER_IO_MAX_REGIONS];
    uint8_t io_region_count;
    DriverDmaBuffer dma_buffers[DRIVER_DMA_MAX_BUFFERS];
    DriverPciMmioRegion pci_mmio_regions[DRIVER_MMIO_MAX_REGIONS];
    int32_t state[DRIVER_STATE_SLOTS];
    uint8_t irq_events[DRIVER_IRQ_QUEUE_SIZE];
    uint8_t irq_event_head;
    uint8_t irq_event_tail;
    uint8_t irq_line;
    uint8_t irq_waiting_ack;
    uint8_t pci_mmio_region_count;
    pci_device_t claimed_device;
    uint16_t original_command;
    uint32_t irq_events_dropped;
    uint8_t has_claimed_device;
    uint8_t irq_subscribed;
    uint8_t allocated;
    uint8_t active;
} DriverRuntimeContext;

static DriverMmioRegion mmio_regions[DRIVER_MMIO_MAX_REGIONS];
static uint8_t mmio_region_count;
static DriverRuntimeContext runtime_contexts[DRIVER_RUNTIME_MAX];
static DriverRuntimeContext* active_runtime;
static int comport_initialized;

#define io_regions (active_runtime->io_regions)
#define io_region_count (active_runtime->io_region_count)
#define dma_buffers (active_runtime->dma_buffers)
#define pci_mmio_regions (active_runtime->pci_mmio_regions)
#define pci_mmio_region_count (active_runtime->pci_mmio_region_count)
#define claimed_pci_device (active_runtime->claimed_device)
#define claimed_original_command (active_runtime->original_command)
#define pci_device_claimed (active_runtime->has_claimed_device)
#define driver_active (active_runtime != 0 && active_runtime->active != 0U)

static uint32_t driver_interrupt_save_disable(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void driver_interrupt_restore(uint32_t flags)
{
    if ((flags & 0x200U) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bdf;
} DriverPciFindContext;

static uint32_t driver_pci_pack_bdf(const pci_device_t* device)
{
    return ((uint32_t)device->bus << 16)
        | ((uint32_t)device->device << 8)
        | (uint32_t)device->function;
}

static int driver_pci_find_visitor(const pci_device_t* device, void* context)
{
    DriverPciFindContext* find = (DriverPciFindContext*)context;

    if (device->vendor_id != find->vendor_id || device->device_id != find->device_id) {
        return 0;
    }

    find->bdf = driver_pci_pack_bdf(device);
    return 1;
}

static int driver_pci_decode_bdf(uint32_t bdf, pci_device_t* out_device)
{
    uint32_t bus;
    uint32_t device;
    uint32_t function;

    if (out_device == 0 || (bdf & 0xFF000000U) != 0U) {
        return -1;
    }

    bus = (bdf >> 16) & 0xFFU;
    device = (bdf >> 8) & 0xFFU;
    function = bdf & 0xFFU;
    if (device >= 32U || function >= 8U) {
        return -1;
    }

    *out_device = (pci_device_t){0};
    out_device->bus = (uint8_t)bus;
    out_device->device = (uint8_t)device;
    out_device->function = (uint8_t)function;
    return 0;
}

static void driver_unmap_pages(uint32_t virtual_base, uint32_t page_count)
{
    for (uint32_t page = 0U; page < page_count; ++page) {
        paging_unmap(virtual_base + page * 4096U);
    }
}

static int driver_find_virtual_pages(uint32_t window_base, uint32_t window_size,
    uint32_t page_count, uint32_t* out_base)
{
    uint32_t bytes = page_count * 4096U;

    if (out_base == 0 || page_count == 0U || bytes > window_size) {
        return -1;
    }

    for (uint32_t candidate = window_base;
        candidate <= window_base + window_size - bytes;
        candidate += 4096U) {
        uint32_t page;
        for (page = 0U; page < page_count; ++page) {
            if (paging_get_physical(candidate + page * 4096U) != 0U) {
                break;
            }
        }
        if (page == page_count) {
            *out_base = candidate;
            return 0;
        }
    }

    return -1;
}

static void driver_clear_pci_mmio_regions(void)
{
    for (uint32_t index = 0U; index < pci_mmio_region_count; ++index) {
        driver_unmap_pages(pci_mmio_regions[index].page_base,
            pci_mmio_regions[index].page_count);
        pci_mmio_regions[index] = (DriverPciMmioRegion){0};
    }
    pci_mmio_region_count = 0U;
}

static void driver_free_dma_buffer(uint32_t handle)
{
    DriverDmaBuffer* buffer = &dma_buffers[handle];

    if (buffer->memory == 0) {
        return;
    }

    driver_unmap_pages(buffer->virtual_base, buffer->page_count);
    pmm_free_pages((void*)(uintptr_t)buffer->physical_base, buffer->page_count);
    *buffer = (DriverDmaBuffer){0};
}

static void driver_runtime_cleanup(DriverRuntimeContext* runtime)
{
    uint32_t flags;

    if (runtime == 0 || runtime->active == 0U) {
        return;
    }

    flags = driver_interrupt_save_disable();
    if (runtime->irq_subscribed != 0U) {
        runtime->irq_subscribed = 0U;
        irq_driver_line_release(runtime->irq_line);
    }
    driver_interrupt_restore(flags);

    /* Recover the kernel COM1 receive interrupt even if VM execution failed. */
    outb(DRIVER_COM1_BASE + 1U, 0x01U);
    if (runtime->has_claimed_device) {
        uint16_t command = pci_config_read16(&runtime->claimed_device, 0x04U);
        command &= (uint16_t)~(DRIVER_PCI_ENABLE_IO
            | DRIVER_PCI_ENABLE_MEMORY
            | DRIVER_PCI_ENABLE_BUS_MASTER);
        command |= (uint16_t)(runtime->original_command
            & (DRIVER_PCI_ENABLE_IO | DRIVER_PCI_ENABLE_MEMORY));
        pci_config_write16(&runtime->claimed_device, 0x04U, command);
        __asm__ volatile ("" : : : "memory");
    }

    active_runtime = runtime;
    driver_clear_pci_mmio_regions();
    for (uint32_t index = 0U; index < DRIVER_DMA_MAX_BUFFERS; ++index) {
        driver_free_dma_buffer(index);
    }
    if (runtime->has_claimed_device) {
        pci_release_device(&runtime->claimed_device, PCI_DEVICE_OWNER_PRISMCC);
        DEBUG_LOG("released resident driver PCI device claim");
    }
    active_runtime = 0;
}

int driver_runtime_create(void)
{
    if (!comport_initialized) {
        if (comport_init() != 0) {
            return -1;
        }
        comport_initialized = 1;
    }

    for (uint32_t index = 0U; index < DRIVER_RUNTIME_MAX; ++index) {
        if (runtime_contexts[index].allocated == 0U) {
            runtime_contexts[index] = (DriverRuntimeContext){0};
            runtime_contexts[index].allocated = 1U;
            runtime_contexts[index].irq_line = 0xFFU;
            return (int)index;
        }
    }

    return -1;
}

int driver_runtime_activate(uint32_t handle)
{
    if (handle >= DRIVER_RUNTIME_MAX
        || runtime_contexts[handle].allocated == 0U) {
        return -1;
    }

    if (active_runtime != 0) {
        active_runtime->active = 0U;
    }
    active_runtime = &runtime_contexts[handle];
    active_runtime->active = 1U;
    return 0;
}

void driver_runtime_deactivate(void)
{
    if (active_runtime != 0) {
        active_runtime->active = 0U;
        active_runtime = 0;
    }
}

void driver_runtime_destroy(uint32_t handle)
{
    DriverRuntimeContext* previous_runtime = active_runtime;
    DriverRuntimeContext* runtime;

    if (handle >= DRIVER_RUNTIME_MAX
        || runtime_contexts[handle].allocated == 0U) {
        return;
    }

    if (previous_runtime != 0) {
        previous_runtime->active = 0U;
    }
    runtime = &runtime_contexts[handle];
    runtime->active = 1U;
    driver_runtime_cleanup(runtime);
    *runtime = (DriverRuntimeContext){0};

    if (previous_runtime != 0 && previous_runtime != runtime
        && previous_runtime->allocated != 0U) {
        previous_runtime->active = 1U;
        active_runtime = previous_runtime;
    } else {
        active_runtime = 0;
    }
}

int driver_runtime_begin(void)
{
    int handle;

    if (active_runtime != 0 && active_runtime->active != 0U) {
        return -1;
    }
    handle = driver_runtime_create();
    if (handle < 0 || driver_runtime_activate((uint32_t)handle) != 0) {
        if (handle >= 0) {
            driver_runtime_destroy((uint32_t)handle);
        }
        return -1;
    }
    return 0;
}

void driver_runtime_end(void)
{
    if (active_runtime != 0) {
        driver_runtime_destroy((uint32_t)(active_runtime - runtime_contexts));
    }
}

int driver_runtime_is_active(void) {
    return driver_active;
}

int driver_api_state_read(uint32_t slot)
{
    if (!driver_active || slot >= DRIVER_STATE_SLOTS) {
        return -1;
    }
    return active_runtime->state[slot];
}

int driver_api_state_write(uint32_t slot, uint32_t value)
{
    if (!driver_active || slot >= DRIVER_STATE_SLOTS) {
        return -1;
    }

    active_runtime->state[slot] = (int32_t)value;
    return 0;
}

int driver_api_irq_read(void)
{
    uint8_t irq;

    if (!driver_active || active_runtime->irq_event_tail == active_runtime->irq_event_head) {
        return -1;
    }

    irq = active_runtime->irq_events[active_runtime->irq_event_tail];
    active_runtime->irq_event_tail = (uint8_t)((active_runtime->irq_event_tail + 1U)
        & (DRIVER_IRQ_QUEUE_SIZE - 1U));
    return irq;
}

int driver_api_irq_dropped(void)
{
    if (!driver_active) {
        return -1;
    }
    return active_runtime->irq_events_dropped > 0x7FFFFFFFU
        ? 0x7FFFFFFF : (int)active_runtime->irq_events_dropped;
}

int driver_api_irq_ack(void)
{
    uint32_t flags;

    if (!driver_active || active_runtime->irq_subscribed == 0U
        || active_runtime->irq_waiting_ack == 0U
        || active_runtime->irq_event_tail != active_runtime->irq_event_head) {
        return -1;
    }

    flags = driver_interrupt_save_disable();
    active_runtime->irq_waiting_ack = 0U;
    irq_driver_line_ack(active_runtime->irq_line);
    driver_interrupt_restore(flags);
    return 0;
}

void driver_runtime_notify_irq(uint8_t irq)
{
    for (uint32_t index = 0U; index < DRIVER_RUNTIME_MAX; ++index) {
        DriverRuntimeContext* runtime = &runtime_contexts[index];
        uint8_t next_head;

        if (runtime->allocated == 0U || runtime->irq_subscribed == 0U
            || runtime->irq_line != irq) {
            continue;
        }

        if (runtime->irq_waiting_ack != 0U) {
            if (runtime->irq_events_dropped < 0x7FFFFFFFU) {
                ++runtime->irq_events_dropped;
            }
            continue;
        }
        next_head = (uint8_t)((runtime->irq_event_head + 1U)
            & (DRIVER_IRQ_QUEUE_SIZE - 1U));
        if (next_head == runtime->irq_event_tail) {
            if (runtime->irq_events_dropped < 0x7FFFFFFFU) {
                ++runtime->irq_events_dropped;
            }
        } else {
            runtime->irq_events[runtime->irq_event_head] = irq;
            runtime->irq_event_head = next_head;
        }
        runtime->irq_waiting_ack = 1U;
        irq_driver_line_mask(irq);
    }
}

int driver_runtime_irq_self_test(void)
{
    DriverRuntimeContext* runtime;
    registers_t regs = {0};
    uint32_t flags;
    int handle;
    int result = -1;

    if (active_runtime != 0) {
        return -1;
    }
    for (uint32_t index = 0U; index < DRIVER_RUNTIME_MAX; ++index) {
        if (runtime_contexts[index].allocated != 0U) {
            return -1;
        }
    }

    handle = driver_runtime_create();
    if (handle < 0 || driver_runtime_activate((uint32_t)handle) != 0) {
        if (handle >= 0) {
            driver_runtime_destroy((uint32_t)handle);
        }
        return -1;
    }

    runtime = &runtime_contexts[handle];
    flags = driver_interrupt_save_disable();
    if (irq_driver_line_acquire(15U) != 0) {
        driver_interrupt_restore(flags);
        goto cleanup;
    }
    runtime->irq_line = 15U;
    runtime->irq_subscribed = 1U;
    driver_interrupt_restore(flags);

    regs.int_no = 47U;
    irq_dispatcher(&regs);
    driver_runtime_notify_irq(15U);
    if (driver_api_irq_read() == 15
        && driver_api_irq_read() == -1
        && driver_api_irq_ack() == 0
        && driver_api_irq_dropped() == 1) {
        result = 0;
    }

cleanup:
    driver_runtime_deactivate();
    driver_runtime_destroy((uint32_t)handle);
    return result;
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

int driver_api_pci_find(uint32_t vendor_id, uint32_t device_id)
{
    DriverPciFindContext context;

    if (!driver_active || vendor_id > 0xFFFFU || device_id > 0xFFFFU) {
        return -1;
    }

    context.vendor_id = (uint16_t)vendor_id;
    context.device_id = (uint16_t)device_id;
    context.bdf = 0xFFFFFFFFU;
    if (pci_scan(driver_pci_find_visitor, &context) < 0 || context.bdf == 0xFFFFFFFFU) {
        return -1;
    }

    return (int)context.bdf;
}

int driver_api_pci_read_config32(uint32_t bdf, uint32_t offset)
{
    pci_device_t device;

    if (!driver_active
        || driver_pci_decode_bdf(bdf, &device) != 0
        || offset > 0xFCU
        || (offset & 3U) != 0U) {
        return -1;
    }

    if ((uint16_t)pci_config_read32(&device, 0U) == 0xFFFFU) {
        return -1;
    }

    return (int)pci_config_read32(&device, (uint8_t)offset);
}

int driver_api_pci_read_bar32(uint32_t bdf, uint32_t bar_index)
{
    pci_device_t device;
    uint64_t base;
    int is_io;

    if (!driver_active
        || driver_pci_decode_bdf(bdf, &device) != 0
        || bar_index > 5U
        || (uint16_t)pci_config_read32(&device, 0U) == 0xFFFFU
        || pci_read_bar(&device, (uint8_t)bar_index, &base, &is_io) != 0
        || base > 0xFFFFFFFFULL) {
        return -1;
    }

    return (int)(uint32_t)base;
}

static int driver_pci_is_claimed(uint32_t bdf, pci_device_t* out_device)
{
    if (!driver_active
        || !pci_device_claimed
        || driver_pci_decode_bdf(bdf, out_device) != 0
        || out_device->bus != claimed_pci_device.bus
        || out_device->device != claimed_pci_device.device
        || out_device->function != claimed_pci_device.function) {
        return 0;
    }
    return 1;
}

int driver_api_pci_claim(uint32_t bdf)
{
    pci_device_t device;
    uint32_t identity;
    uint32_t class_data;
    uint32_t header_data;
    uint32_t interrupt_data;

    if (!driver_active || pci_device_claimed
        || driver_pci_decode_bdf(bdf, &device) != 0) {
        return -1;
    }

    identity = pci_config_read32(&device, 0U);
    if ((uint16_t)identity == 0xFFFFU) {
        return -1;
    }
    class_data = pci_config_read32(&device, 0x08U);
    header_data = pci_config_read32(&device, 0x0CU);
    interrupt_data = pci_config_read32(&device, 0x3CU);
    device.vendor_id = (uint16_t)identity;
    device.device_id = (uint16_t)(identity >> 16);
    device.class_code = (uint8_t)(class_data >> 24);
    device.subclass = (uint8_t)(class_data >> 16);
    device.programming_interface = (uint8_t)(class_data >> 8);
    device.interrupt_line = (uint8_t)interrupt_data;
    device.header_type = (uint8_t)(header_data >> 16);

    if (pci_claim_device(&device, PCI_DEVICE_OWNER_PRISMCC) != 0) {
        return -1;
    }

    claimed_original_command = pci_config_read16(&device, 0x04U);
    claimed_pci_device = device;
    pci_device_claimed = 1;
    return 0;
}

int driver_api_pci_subscribe_irq(uint32_t bdf)
{
    pci_device_t device;
    uint32_t interrupt_data;
    uint32_t flags;
    uint8_t irq;
    uint8_t pin;

    if (!driver_active || !driver_pci_is_claimed(bdf, &device)) {
        return -1;
    }
    if (active_runtime->irq_subscribed != 0U) {
        return active_runtime->irq_line;
    }

    interrupt_data = pci_config_read32(&device, 0x3CU);
    irq = (uint8_t)interrupt_data;
    pin = (uint8_t)(interrupt_data >> 8);
    if (pin == 0U || irq == 0xFFU) {
        return -1;
    }

    flags = driver_interrupt_save_disable();
    active_runtime->irq_line = irq;
    active_runtime->irq_event_head = 0U;
    active_runtime->irq_event_tail = 0U;
    active_runtime->irq_events_dropped = 0U;
    active_runtime->irq_waiting_ack = 0U;
    if (irq_driver_line_acquire(irq) != 0) {
        active_runtime->irq_line = 0xFFU;
        driver_interrupt_restore(flags);
        return -1;
    }
    active_runtime->irq_subscribed = 1U;
    driver_interrupt_restore(flags);
    return irq;
}

int driver_api_pci_enable(uint32_t bdf, uint32_t flags)
{
    pci_device_t device;
    uint16_t command;

    if ((flags & ~(DRIVER_PCI_ENABLE_IO
            | DRIVER_PCI_ENABLE_MEMORY
            | DRIVER_PCI_ENABLE_BUS_MASTER)) != 0U
        || !driver_pci_is_claimed(bdf, &device)) {
        return -1;
    }

    command = pci_config_read16(&device, 0x04U);
    command |= (uint16_t)flags;
    pci_config_write16(&device, 0x04U, command);
    return 0;
}

int driver_api_pci_disable(uint32_t bdf, uint32_t flags)
{
    pci_device_t device;
    uint16_t command;

    if ((flags & ~(DRIVER_PCI_ENABLE_IO
            | DRIVER_PCI_ENABLE_MEMORY
            | DRIVER_PCI_ENABLE_BUS_MASTER)) != 0U
        || !driver_pci_is_claimed(bdf, &device)) {
        return -1;
    }

    command = pci_config_read16(&device, 0x04U);
    command &= (uint16_t)~flags;
    pci_config_write16(&device, 0x04U, command);
    __asm__ volatile ("" : : : "memory");
    return 0;
}

int driver_api_pci_claim_io_bar(uint32_t bdf, uint32_t bar_index)
{
    pci_device_t device;
    uint64_t base;
    uint64_t size;
    int is_io;

    if (!driver_pci_is_claimed(bdf, &device)
        || bar_index > 5U
        || io_region_count >= DRIVER_IO_MAX_REGIONS
        || (pci_config_read16(&device, 0x04U) & DRIVER_PCI_ENABLE_IO) == 0U
        || (pci_config_read16(&device, 0x04U) & DRIVER_PCI_ENABLE_BUS_MASTER) != 0U
        || pci_read_bar_info(&device, (uint8_t)bar_index, &base, &size, &is_io) != 0
        || !is_io
        || size > 0x10000U) {
        return -1;
    }

    for (uint32_t index = 0U; index < io_region_count; ++index) {
        uint32_t existing_end = (uint32_t)io_regions[index].base + io_regions[index].size;
        uint32_t requested_end = (uint32_t)base + (uint32_t)size;
        if ((uint32_t)base < existing_end && (uint32_t)io_regions[index].base < requested_end) {
            return -1;
        }
    }
    if ((uint32_t)base < DRIVER_COM1_BASE + DRIVER_COM1_PORT_COUNT
        && DRIVER_COM1_BASE < (uint32_t)base + (uint32_t)size) {
        return -1;
    }

    io_regions[io_region_count].base = (uint16_t)base;
    io_regions[io_region_count].size = (uint32_t)size;
    return io_region_count++;
}

int driver_api_pci_claim_mmio_bar(uint32_t bdf, uint32_t bar_index)
{
    pci_device_t device;
    uint64_t physical_base;
    uint64_t size;
    uint64_t physical_end;
    uint32_t physical_page;
    uint32_t page_offset;
    uint32_t mapped_size;
    uint32_t page_count;
    uint32_t virtual_page;
    int is_io;

    if (!driver_pci_is_claimed(bdf, &device)
        || bar_index > 5U
        || pci_mmio_region_count >= DRIVER_MMIO_MAX_REGIONS
        || (pci_config_read16(&device, 0x04U) & DRIVER_PCI_ENABLE_MEMORY) == 0U
        || (pci_config_read16(&device, 0x04U) & DRIVER_PCI_ENABLE_BUS_MASTER) != 0U
        || pci_read_bar_info(&device, (uint8_t)bar_index,
            &physical_base, &size, &is_io) != 0
        || is_io
        || physical_base == 0U
        || physical_base > 0xFFFFFFFFULL
        || (physical_base & 0xFFFFF000ULL) == 0U
        || size == 0U
        || size > DRIVER_PCI_MMIO_MAX_SIZE) {
        return -1;
    }

    physical_end = physical_base + size;
    if (physical_end > 0x100000000ULL || physical_end < physical_base) {
        return -1;
    }

    for (uint32_t index = 0U; index < pci_mmio_region_count; ++index) {
        uint64_t existing_start = pci_mmio_regions[index].physical_base;
        uint64_t existing_end = existing_start + pci_mmio_regions[index].size;
        if (physical_base < existing_end && existing_start < physical_end) {
            return -1;
        }
    }

    physical_page = (uint32_t)physical_base & 0xFFFFF000U;
    page_offset = (uint32_t)physical_base & 0xFFFU;
    if (size + page_offset > 0xFFFFFFFFULL) {
        return -1;
    }
    mapped_size = (uint32_t)(size + page_offset);
    page_count = (mapped_size + 4095U) / 4096U;
    if (driver_find_virtual_pages(DRIVER_PCI_MMIO_VIRTUAL_BASE,
            DRIVER_PCI_MMIO_VIRTUAL_SIZE, page_count, &virtual_page) != 0) {
        return -1;
    }

    for (uint32_t page = 0U; page < page_count; ++page) {
        uint32_t virtual_address = virtual_page + page * 4096U;
        uint32_t physical_address = physical_page + page * 4096U;
        paging_map(virtual_address, physical_address,
            PAGING_FLAG_READ_WRITE | PAGING_FLAG_WRITE_THROUGH | PAGING_FLAG_CACHE_DISABLE);
        if (paging_get_physical(virtual_address) != physical_address) {
            driver_unmap_pages(virtual_page, page + 1U);
            return -1;
        }
    }

    pci_mmio_regions[pci_mmio_region_count] = (DriverPciMmioRegion){
        virtual_page + page_offset,
        (uint32_t)size,
        virtual_page,
        page_count,
        (uint32_t)physical_base
    };
    return pci_mmio_region_count++;
}

int driver_api_pci_mmio_read32(uint32_t region, uint32_t offset)
{
    if (!driver_active || region >= pci_mmio_region_count
        || !pci_device_claimed
        || (pci_config_read16(&claimed_pci_device, 0x04U) & DRIVER_PCI_ENABLE_MEMORY) == 0U
        || (offset & 3U) != 0U
        || offset > pci_mmio_regions[region].size
        || sizeof(uint32_t) > pci_mmio_regions[region].size - offset) {
        return -1;
    }

    __asm__ volatile ("" : : : "memory");
    return (int)*(volatile uint32_t*)(uintptr_t)(
        pci_mmio_regions[region].mapped_base + offset);
}

int driver_api_pci_mmio_write32(uint32_t region, uint32_t offset, uint32_t value)
{
    if (!driver_active || region >= pci_mmio_region_count
        || !pci_device_claimed
        || (pci_config_read16(&claimed_pci_device, 0x04U) & DRIVER_PCI_ENABLE_MEMORY) == 0U
        || (offset & 3U) != 0U
        || offset > pci_mmio_regions[region].size
        || sizeof(uint32_t) > pci_mmio_regions[region].size - offset) {
        return -1;
    }

    *(volatile uint32_t*)(uintptr_t)(
        pci_mmio_regions[region].mapped_base + offset) = value;
    __asm__ volatile ("" : : : "memory");
    return 0;
}

static int driver_pci_io_validate(uint32_t region, uint32_t offset, uint32_t width,
    uint16_t* out_port)
{
    uint32_t end;

    if (!driver_active || region >= io_region_count || out_port == 0
        || !pci_device_claimed
        || (pci_config_read16(&claimed_pci_device, 0x04U) & DRIVER_PCI_ENABLE_IO) == 0U
        || (width != 1U && width != 2U && width != 4U)
        || (offset & (width - 1U)) != 0U
        || offset > io_regions[region].size
        || width > io_regions[region].size - offset) {
        return -1;
    }

    end = (uint32_t)io_regions[region].base + offset + width;
    if (end > 0x10000U) {
        return -1;
    }

    *out_port = (uint16_t)(io_regions[region].base + offset);
    return 0;
}

int driver_api_pci_io_read(uint32_t region, uint32_t offset, uint32_t width)
{
    uint16_t port;
    if (driver_pci_io_validate(region, offset, width, &port) != 0) {
        return -1;
    }

    if (width == 1U) {
        return (int)inb(port);
    }
    if (width == 2U) {
        return (int)inw(port);
    }
    return (int)inl(port);
}

int driver_api_pci_io_write(uint32_t region, uint32_t offset, uint32_t width, uint32_t value)
{
    uint16_t port;
    if (driver_pci_io_validate(region, offset, width, &port) != 0
        || (width == 1U && value > 0xFFU)
        || (width == 2U && value > 0xFFFFU)) {
        return -1;
    }

    if (width == 1U) {
        outb(port, (uint8_t)value);
    } else if (width == 2U) {
        outw(port, (uint16_t)value);
    } else {
        outl(port, value);
    }
    return 0;
}

static int driver_dma_alloc_with_limit(uint32_t size, uint64_t max_address_exclusive)
{
    uint32_t page_count;

    if (!driver_active || size == 0U || size > DRIVER_DMA_MAX_SIZE
        || max_address_exclusive == 0U
        || max_address_exclusive > DRIVER_DMA_ADDRESS_LIMIT) {
        return -1;
    }

    page_count = (size + 4095U) / 4096U;
    for (uint32_t index = 0U; index < DRIVER_DMA_MAX_BUFFERS; ++index) {
        if (dma_buffers[index].memory != 0) {
            continue;
        }

        uint8_t* physical_memory = (uint8_t*)pmm_alloc_pages_below(page_count,
            max_address_exclusive);
        uint32_t physical_base;
        uint32_t virtual_base;
        if (physical_memory == 0) {
            return -1;
        }

        physical_base = (uint32_t)(uintptr_t)physical_memory;
        if (driver_find_virtual_pages(DRIVER_DMA_VIRTUAL_BASE,
                DRIVER_DMA_VIRTUAL_SIZE, page_count, &virtual_base) != 0) {
            pmm_free_pages(physical_memory, page_count);
            return -1;
        }

        for (uint32_t page = 0U; page < page_count; ++page) {
            uint32_t virtual_address = virtual_base + page * 4096U;
            uint32_t physical_address = physical_base + page * 4096U;
            paging_map(virtual_address, physical_address, PAGING_FLAG_READ_WRITE);
            if (paging_get_physical(virtual_address) != physical_address) {
                driver_unmap_pages(virtual_base, page + 1U);
                pmm_free_pages(physical_memory, page_count);
                return -1;
            }
        }

        uint8_t* memory = (uint8_t*)(uintptr_t)virtual_base;
        for (uint32_t byte = 0U; byte < page_count * 4096U; ++byte) {
            memory[byte] = 0U;
        }
        dma_buffers[index].memory = memory;
        dma_buffers[index].physical_base = physical_base;
        dma_buffers[index].virtual_base = virtual_base;
        dma_buffers[index].size = size;
        dma_buffers[index].page_count = page_count;
        return (int)index;
    }

    return -1;
}

int driver_api_dma_alloc(uint32_t size)
{
    return driver_dma_alloc_with_limit(size, DRIVER_DMA_ADDRESS_LIMIT);
}

int driver_api_dma_alloc_below(uint32_t size, uint32_t max_address_exclusive)
{
    return driver_dma_alloc_with_limit(size, max_address_exclusive);
}

static int driver_dma_validate(uint32_t handle, uint32_t offset, uint32_t width)
{
    if (!driver_active || handle >= DRIVER_DMA_MAX_BUFFERS
        || dma_buffers[handle].memory == 0
        || (width != 1U && width != 2U && width != 4U)
        || (offset & (width - 1U)) != 0U
        || offset > dma_buffers[handle].size
        || width > dma_buffers[handle].size - offset) {
        return -1;
    }
    return 0;
}

int driver_api_dma_physical(uint32_t handle)
{
    if (!driver_active || handle >= DRIVER_DMA_MAX_BUFFERS
        || dma_buffers[handle].memory == 0) {
        return -1;
    }
    return (int)dma_buffers[handle].physical_base;
}

int driver_api_dma_read(uint32_t handle, uint32_t offset, uint32_t width)
{
    volatile uint8_t* memory;

    if (driver_dma_validate(handle, offset, width) != 0) {
        return -1;
    }
    memory = dma_buffers[handle].memory;
    __asm__ volatile ("" : : : "memory");

    if (width == 1U) {
        return memory[offset];
    }
    if (width == 2U) {
        return (int)((uint32_t)memory[offset] | ((uint32_t)memory[offset + 1U] << 8));
    }
    return (int)((uint32_t)memory[offset]
        | ((uint32_t)memory[offset + 1U] << 8)
        | ((uint32_t)memory[offset + 2U] << 16)
        | ((uint32_t)memory[offset + 3U] << 24));
}

int driver_api_dma_write(uint32_t handle, uint32_t offset, uint32_t width, uint32_t value)
{
    volatile uint8_t* memory;

    if (driver_dma_validate(handle, offset, width) != 0
        || (width == 1U && value > 0xFFU)
        || (width == 2U && value > 0xFFFFU)) {
        return -1;
    }

    memory = dma_buffers[handle].memory;
    memory[offset] = (uint8_t)value;
    if (width >= 2U) {
        memory[offset + 1U] = (uint8_t)(value >> 8);
    }
    if (width == 4U) {
        memory[offset + 2U] = (uint8_t)(value >> 16);
        memory[offset + 3U] = (uint8_t)(value >> 24);
    }
    __asm__ volatile ("" : : : "memory");
    return 0;
}

int driver_api_dma_free(uint32_t handle)
{
    if (!driver_active || handle >= DRIVER_DMA_MAX_BUFFERS
        || dma_buffers[handle].memory == 0) {
        return -1;
    }
    if (pci_device_claimed
        && (pci_config_read16(&claimed_pci_device, 0x04U)
            & DRIVER_PCI_ENABLE_BUS_MASTER) != 0U) {
        return -1;
    }

    driver_free_dma_buffer(handle);
    return 0;
}
