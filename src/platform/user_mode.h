#ifndef PRISMOS_PLATFORM_USER_MODE_H
#define PRISMOS_PLATFORM_USER_MODE_H

#include <stdint.h>

#include "interrupts/irq.h"
#include "memory/paging.h"

typedef enum {
    USER_SYSCALL_EXIT = 1,
    USER_SYSCALL_DRIVER_SERIAL_WRITE = 0x100,
    USER_SYSCALL_DRIVER_SERIAL_READ,
    USER_SYSCALL_DRIVER_PORT_READ8,
    USER_SYSCALL_DRIVER_PORT_WRITE8,
    USER_SYSCALL_DRIVER_MMIO_READ32,
    USER_SYSCALL_DRIVER_MMIO_WRITE32,
    USER_SYSCALL_DRIVER_SHOULD_STOP,
    USER_SYSCALL_DRIVER_PCI_FIND,
    USER_SYSCALL_DRIVER_PCI_READ_CONFIG32,
    USER_SYSCALL_DRIVER_PCI_READ_BAR32,
    USER_SYSCALL_DRIVER_PCI_CLAIM,
    USER_SYSCALL_DRIVER_PCI_ENABLE,
    USER_SYSCALL_DRIVER_PCI_DISABLE,
    USER_SYSCALL_DRIVER_PCI_CLAIM_IO_BAR,
    USER_SYSCALL_DRIVER_PCI_IO_READ,
    USER_SYSCALL_DRIVER_PCI_IO_WRITE,
    USER_SYSCALL_DRIVER_PCI_CLAIM_MMIO_BAR,
    USER_SYSCALL_DRIVER_PCI_MMIO_READ32,
    USER_SYSCALL_DRIVER_PCI_MMIO_WRITE32,
    USER_SYSCALL_DRIVER_DMA_ALLOC,
    USER_SYSCALL_DRIVER_DMA_ALLOC_BELOW,
    USER_SYSCALL_DRIVER_DMA_PHYSICAL,
    USER_SYSCALL_DRIVER_DMA_READ,
    USER_SYSCALL_DRIVER_DMA_WRITE,
    USER_SYSCALL_DRIVER_DMA_FREE,
    USER_SYSCALL_DRIVER_STATE_READ,
    USER_SYSCALL_DRIVER_STATE_WRITE,
    USER_SYSCALL_DRIVER_CONSOLE_WRITE,
    USER_SYSCALL_DRIVER_CONSOLE_WRITE_INT,
    USER_SYSCALL_DRIVER_CONSOLE_WRITE_CHAR,
    USER_SYSCALL_DRIVER_CONSOLE_WRITE_NL,
    USER_SYSCALL_DRIVER_PCI_SUBSCRIBE_IRQ,
    USER_SYSCALL_DRIVER_IRQ_READ,
    USER_SYSCALL_DRIVER_IRQ_DROPPED,
    USER_SYSCALL_DRIVER_IRQ_ACK,
    USER_SYSCALL_DRIVER_PREEMPT_CHECK
} user_syscall_number_t;

typedef struct {
    uint32_t kernel_esp;
    int32_t status;
    uint32_t saved_eflags;
    uint32_t active;
    uint8_t address_space_slot;
    uint8_t driver_services_enabled;
    uint8_t stop_requested;
    uint8_t driver_runtime_handle;
    volatile uint8_t preempt_requested;
} user_mode_context_t;

int user_mode_enter(user_mode_context_t* context,
    const paging_user_space_t* address_space, uint32_t entry,
    uint32_t user_stack, uint32_t kernel_stack_top);
int user_mode_handle_exception(registers_t* regs);
int user_mode_run_self_test(void);
int user_mode_isolation_available(void);
void user_mode_request_stop(user_mode_context_t* context);
void user_mode_request_preempt(registers_t* regs);
void syscall_dispatcher(registers_t* regs);

#endif
