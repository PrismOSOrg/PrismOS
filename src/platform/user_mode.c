#include "platform/user_mode.h"

#include <stddef.h>

#include "debug/log.h"
#include "apps/app_manager.h"
#include "comport/comport.h"
#include "memory/paging.h"
#include "memory/pmm.h"
#include "platform/gdt.h"
#include "platform/io.h"
#include "drivers/driver_api.h"
#include "input/keyboard.h"

#define USER_KERNEL_STACK_SIZE 4096U
#define USER_TEST_PHYSICAL_LIMIT 0x04000000ULL
#define USER_SYSCALL_STRING_MAX 4096U

extern void user_mode_enter_asm(user_mode_context_t* context,
    uint32_t entry, uint32_t user_stack);
extern void user_mode_return_trampoline(void);
extern uint8_t __user_vm_start;
extern uint8_t __user_vm_end;
extern uint8_t __user_vm_load_start;

uint32_t user_mode_return_stack;

static user_mode_context_t* active_user_context;
static uint8_t user_mode_kernel_stack[USER_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));
static char syscall_string_buffer[USER_SYSCALL_STRING_MAX];
static int isolation_self_test_passed;

static void user_mode_log_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    comport_write_string("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        comport_write_char(digits[(value >> shift) & 0x0FU]);
    }
}

static void user_mode_console_write_int(int32_t value)
{
    char buffer[12];
    uint32_t magnitude = (uint32_t)value;
    uint32_t length = 0U;
    uint32_t index;

    if (value < 0) {
        magnitude = 0U - magnitude;
    }
    do {
        buffer[length++] = (char)('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude != 0U);
    if (value < 0) {
        buffer[length++] = '-';
    }

    for (index = 0U; index < length / 2U; ++index) {
        char temp = buffer[index];
        buffer[index] = buffer[length - index - 1U];
        buffer[length - index - 1U] = temp;
    }
    buffer[length] = '\0';
    console_write(buffer);
}

static void user_mode_return_to_kernel(registers_t* regs, int32_t status)
{
    user_mode_context_t* context = active_user_context;

    if (context == 0 || context->active == 0U) {
        return;
    }

    context->status = status;
    user_mode_return_stack = context->kernel_esp;
    regs->eax = (uint32_t)status;
    regs->eip = (uint32_t)(uintptr_t)user_mode_return_trampoline;
    regs->cs = GDT_KERNEL_CODE_SELECTOR;
    regs->eflags = context->saved_eflags;
}

int user_mode_enter(user_mode_context_t* context,
    const paging_user_space_t* address_space, uint32_t entry,
    uint32_t user_stack, uint32_t kernel_stack_top)
{
    uint32_t previous_kernel_stack;
    uint32_t flags;

    if (context == 0 || !paging_user_space_is_active(address_space)
        || active_user_context != 0
        || entry < PAGING_USER_BASE
        || entry >= PAGING_USER_BASE + PAGING_USER_SIZE
        || user_stack <= PAGING_USER_BASE
        || user_stack > PAGING_USER_BASE + PAGING_USER_SIZE
        || kernel_stack_top == 0U
        || (kernel_stack_top & 0x0FU) != 0U) {
        return -1;
    }

    if (context->driver_services_enabled != 0U
        && driver_runtime_activate(context->driver_runtime_handle) != 0) {
        return -1;
    }

    __asm__ volatile ("pushfl; popl %0" : "=r"(flags) : : "memory");
    context->kernel_esp = 0U;
    context->status = -1;
    context->saved_eflags = (flags & ~(0x3000U | 0x4000U | 0x20000U)) | 0x02U;
    context->active = 1U;
    context->preempt_requested = 0U;
    context->address_space_slot = address_space->slot;
    active_user_context = context;
    previous_kernel_stack = gdt_get_kernel_stack();
    gdt_set_kernel_stack(kernel_stack_top);

    user_mode_enter_asm(context, entry, user_stack);

    gdt_set_kernel_stack(previous_kernel_stack);
    if (context->driver_services_enabled != 0U) {
        driver_runtime_deactivate();
    }
    active_user_context = 0;
    context->active = 0U;
    return context->status;
}

void syscall_dispatcher(registers_t* regs)
{
    user_mode_context_t* context;
    int result = -1;

    if (regs == 0 || (regs->cs & 3U) != 3U
        || active_user_context == 0
        || active_user_context->active == 0U) {
        if (regs != 0) {
            regs->eax = (uint32_t)-1;
        }
        return;
    }

    context = active_user_context;
    if (regs->eax == USER_SYSCALL_EXIT) {
        user_mode_return_to_kernel(regs, (int32_t)regs->ebx);
        return;
    }

    if (context->driver_services_enabled == 0U) {
        regs->eax = (uint32_t)-1;
        return;
    }

    switch (regs->eax) {
        case USER_SYSCALL_DRIVER_SERIAL_WRITE: {
            paging_user_space_t space = {context->address_space_slot};
            uint32_t length = regs->ecx;
            if (length <= USER_SYSCALL_STRING_MAX
                && paging_user_space_copy_from(&space, syscall_string_buffer,
                    regs->ebx, length) == 0) {
                result = driver_api_serial_write(syscall_string_buffer, length);
            }
            break;
        }
        case USER_SYSCALL_DRIVER_SERIAL_READ:
            result = driver_api_serial_read();
            break;
        case USER_SYSCALL_DRIVER_CONSOLE_WRITE: {
            paging_user_space_t space = {context->address_space_slot};
            uint32_t length = regs->ecx;
            if (length < USER_SYSCALL_STRING_MAX
                && paging_user_space_copy_from(&space, syscall_string_buffer,
                    regs->ebx, length) == 0) {
                syscall_string_buffer[length] = '\0';
                console_write(syscall_string_buffer);
                result = (int)length;
            }
            break;
        }
        case USER_SYSCALL_DRIVER_CONSOLE_WRITE_INT:
            user_mode_console_write_int((int32_t)regs->ebx);
            result = 0;
            break;
        case USER_SYSCALL_DRIVER_CONSOLE_WRITE_CHAR:
            console_write_char((char)regs->ebx);
            result = 0;
            break;
        case USER_SYSCALL_DRIVER_CONSOLE_WRITE_NL:
            console_write_char('\n');
            result = 0;
            break;
        case USER_SYSCALL_DRIVER_PORT_READ8:
            result = driver_api_port_read8(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_PORT_WRITE8:
            result = driver_api_port_write8(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_MMIO_READ32:
            result = driver_api_mmio_read32(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_MMIO_WRITE32:
            result = driver_api_mmio_write32(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_SHOULD_STOP: {
            KeyEvent event;
            if (context->stop_requested != 0U) {
                result = 1;
                break;
            }
            event = keyboard_peek_event();
            if (event.type == KEY_EVENT_CHARACTER && event.character == 27
                && !app_manager_foreground_app_active()) {
                (void)keyboard_poll_event();
                DEBUG_LOG("resident driver received Escape stop event");
                context->stop_requested = 1U;
                result = 1;
            } else {
                result = 0;
            }
            break;
        }
        case USER_SYSCALL_DRIVER_PCI_FIND:
            result = driver_api_pci_find(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_READ_CONFIG32:
            result = driver_api_pci_read_config32(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_READ_BAR32:
            result = driver_api_pci_read_bar32(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_CLAIM:
            result = driver_api_pci_claim(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_PCI_ENABLE:
            result = driver_api_pci_enable(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_DISABLE:
            result = driver_api_pci_disable(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_CLAIM_IO_BAR:
            result = driver_api_pci_claim_io_bar(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_IO_READ:
            result = driver_api_pci_io_read(regs->ebx, regs->ecx, regs->edx);
            break;
        case USER_SYSCALL_DRIVER_PCI_IO_WRITE:
            result = driver_api_pci_io_write(regs->ebx, regs->ecx,
                regs->edx, regs->esi);
            break;
        case USER_SYSCALL_DRIVER_PCI_CLAIM_MMIO_BAR:
            result = driver_api_pci_claim_mmio_bar(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_MMIO_READ32:
            result = driver_api_pci_mmio_read32(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_MMIO_WRITE32:
            result = driver_api_pci_mmio_write32(regs->ebx, regs->ecx, regs->edx);
            break;
        case USER_SYSCALL_DRIVER_DMA_ALLOC:
            result = driver_api_dma_alloc(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_DMA_ALLOC_BELOW:
            result = driver_api_dma_alloc_below(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_DMA_PHYSICAL:
            result = driver_api_dma_physical(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_DMA_READ:
            result = driver_api_dma_read(regs->ebx, regs->ecx, regs->edx);
            break;
        case USER_SYSCALL_DRIVER_DMA_WRITE:
            result = driver_api_dma_write(regs->ebx, regs->ecx,
                regs->edx, regs->esi);
            break;
        case USER_SYSCALL_DRIVER_DMA_FREE:
            result = driver_api_dma_free(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_STATE_READ:
            result = driver_api_state_read(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_STATE_WRITE:
            result = driver_api_state_write(regs->ebx, regs->ecx);
            break;
        case USER_SYSCALL_DRIVER_PCI_SUBSCRIBE_IRQ:
            result = driver_api_pci_subscribe_irq(regs->ebx);
            break;
        case USER_SYSCALL_DRIVER_IRQ_READ:
            result = driver_api_irq_read();
            break;
        case USER_SYSCALL_DRIVER_IRQ_DROPPED:
            result = driver_api_irq_dropped();
            break;
        case USER_SYSCALL_DRIVER_IRQ_ACK:
            result = driver_api_irq_ack();
            break;
        case USER_SYSCALL_DRIVER_PREEMPT_CHECK:
            result = context->preempt_requested != 0U ? 1 : 0;
            context->preempt_requested = 0U;
            break;
        default:
            result = -1;
            break;
    }
    regs->eax = (uint32_t)result;
}

int user_mode_handle_exception(registers_t* regs)
{
    if (regs == 0 || (regs->cs & 3U) != 3U || active_user_context == 0) {
        return 0;
    }

    if (active_user_context->driver_services_enabled != 0U) {
        if (regs->int_no == 14U) {
            ERROR_LOG("isolated driver page fault");
            comport_write_string("fault eip=");
            user_mode_log_hex(regs->eip);
            comport_write_string(" cr2=");
            user_mode_log_hex(read_cr2());
            comport_write_string(" error=");
            user_mode_log_hex(regs->err_code);
            comport_write_char('\n');
        } else if (regs->int_no == 13U) {
            ERROR_LOG("isolated driver general-protection fault");
        } else if (regs->int_no == 6U) {
            ERROR_LOG("isolated driver invalid opcode");
        } else if (regs->int_no == 0U) {
            ERROR_LOG("isolated driver divide error");
        } else {
            ERROR_LOG("isolated driver CPU exception");
        }
    }
    user_mode_return_to_kernel(regs, -2);
    return 1;
}

static void user_mode_copy_program(uint32_t physical_page,
    const uint8_t* program, uint32_t length)
{
    volatile uint8_t* destination = (volatile uint8_t*)(uintptr_t)physical_page;

    for (uint32_t index = 0U; index < length; ++index) {
        destination[index] = program[index];
    }
}

int user_mode_run_self_test(void)
{
    static const uint8_t exit_program[] = {
        0xB8U, (uint8_t)USER_SYSCALL_EXIT, 0U, 0U, 0U,
        0xBBU, 42U, 0U, 0U, 0U,
        0xCDU, 0x80U,
        0x0FU, 0x0BU
    };
    static const uint8_t fault_program[] = {
        0xA1U, 0U, 0U, 0x10U, 0U,
        0xB8U, (uint8_t)USER_SYSCALL_EXIT, 0U, 0U, 0U,
        0xBBU, 43U, 0U, 0U, 0U,
        0xCDU, 0x80U,
        0x0FU, 0x0BU
    };
    static const uint8_t io_program[] = {
        0x66U, 0xBAU, 0xF8U, 0x03U,
        0xB0U, 0xFFU,
        0xEEU
    };
    static const uint8_t preempt_program[] = {
        0xB8U, 0x23U, 0x01U, 0U, 0U,
        0xCDU, 0x80U,
        0x85U, 0xC0U,
        0x74U, 0xF5U,
        0xB8U, (uint8_t)USER_SYSCALL_EXIT, 0U, 0U, 0U,
        0xBBU, 44U, 0U, 0U, 0U,
        0xCDU, 0x80U,
        0x0FU, 0x0BU
    };
    paging_user_space_t space;
    user_mode_context_t context = {0};
    void* code_page = 0;
    void* stack_page = 0;
    uint8_t verify_program[sizeof(exit_program)];
    uint8_t stack_marker = 0x5AU;
    uint8_t copied_marker = 0U;
    uint32_t user_vm_start = (uint32_t)(uintptr_t)&__user_vm_start;
    uint32_t user_vm_end = (uint32_t)(uintptr_t)&__user_vm_end;
    uint32_t user_vm_load_start = (uint32_t)(uintptr_t)&__user_vm_load_start;
    uint32_t test_code_address;
    int preempt_runtime = -1;
    int space_created = 0;
    int space_active = 0;
    int result = -1;
    uint32_t kernel_stack_top =
        (uint32_t)(uintptr_t)(user_mode_kernel_stack + sizeof(user_mode_kernel_stack));

    isolation_self_test_passed = 0;
    code_page = pmm_alloc_pages_below(1U, USER_TEST_PHYSICAL_LIMIT);
    stack_page = pmm_alloc_pages_below(1U, USER_TEST_PHYSICAL_LIMIT);
    if (code_page == 0 || stack_page == 0
        || user_vm_start != PAGING_USER_BASE
        || user_vm_end <= user_vm_start
        || user_vm_end > PAGING_USER_BASE + PAGING_USER_SIZE
        || paging_user_space_create(&space) != 0) {
        goto cleanup;
    }
    space_created = 1;

    for (uint32_t offset = 0U; offset < user_vm_end - user_vm_start; offset += 4096U) {
        if (paging_user_space_map(&space, user_vm_start + offset,
                user_vm_load_start + offset, 0U) != 0) {
            goto cleanup;
        }
    }
    test_code_address = user_vm_end;
    if (paging_user_space_map(&space, test_code_address,
            (uint32_t)(uintptr_t)code_page, 0U) != 0
        || paging_user_space_map(&space, test_code_address + 4096U,
            (uint32_t)(uintptr_t)stack_page, PAGING_FLAG_READ_WRITE) != 0) {
        goto cleanup;
    }

    user_mode_copy_program((uint32_t)(uintptr_t)code_page,
        exit_program, sizeof(exit_program));
    if (paging_user_space_activate(&space) != 0) {
        goto cleanup;
    }
    space_active = 1;

    if (paging_user_space_copy_from(&space, verify_program,
            test_code_address, sizeof(verify_program)) != 0
        || paging_user_space_copy_to(&space, test_code_address,
            &stack_marker, sizeof(stack_marker)) == 0
        || paging_user_space_copy_to(&space, test_code_address + 4096U,
            &stack_marker, sizeof(stack_marker)) != 0
        || paging_user_space_copy_from(&space, &copied_marker,
            test_code_address + 4096U, sizeof(copied_marker)) != 0
        || copied_marker != stack_marker) {
        goto cleanup;
    }
    for (uint32_t index = 0U; index < sizeof(exit_program); ++index) {
        if (verify_program[index] != exit_program[index]) {
            goto cleanup;
        }
    }
    if (user_mode_enter(&context, &space, test_code_address,
            test_code_address + 8192U - 16U, kernel_stack_top) != 42) {
        goto cleanup;
    }

    user_mode_copy_program((uint32_t)(uintptr_t)code_page,
        fault_program, sizeof(fault_program));
    if (user_mode_enter(&context, &space, test_code_address,
            test_code_address + 8192U - 16U, kernel_stack_top) != -2) {
        goto cleanup;
    }

    user_mode_copy_program((uint32_t)(uintptr_t)code_page,
        io_program, sizeof(io_program));
    if (user_mode_enter(&context, &space, test_code_address,
            test_code_address + 8192U - 16U, kernel_stack_top) != -2) {
        goto cleanup;
    }

    preempt_runtime = driver_runtime_create();
    if (preempt_runtime < 0) {
        goto cleanup;
    }
    context.driver_services_enabled = 1U;
    context.driver_runtime_handle = (uint8_t)preempt_runtime;
    user_mode_copy_program((uint32_t)(uintptr_t)code_page,
        preempt_program, sizeof(preempt_program));
    if (user_mode_enter(&context, &space, test_code_address,
            test_code_address + 8192U - 16U, kernel_stack_top) != 44) {
        goto cleanup;
    }
    context.driver_services_enabled = 0U;

    result = 0;

cleanup:
    if (space_active) {
        paging_kernel_space_activate();
    }
    if (preempt_runtime >= 0) {
        driver_runtime_destroy((uint32_t)preempt_runtime);
    }
    if (space_created) {
        paging_user_space_destroy(&space);
    }
    if (code_page != 0) {
        pmm_free_pages(code_page, 1U);
    }
    if (stack_page != 0) {
        pmm_free_pages(stack_page, 1U);
    }
    isolation_self_test_passed = result == 0;
    return result;
}

int user_mode_isolation_available(void)
{
    return isolation_self_test_passed;
}

void user_mode_request_stop(user_mode_context_t* context)
{
    if (context != 0) {
        context->stop_requested = 1U;
    }
}

void user_mode_request_preempt(registers_t* regs)
{
    user_mode_context_t* context = active_user_context;

    if (regs != 0 && (regs->cs & 3U) == 3U && context != 0
        && context->active != 0U
        && context->driver_services_enabled != 0U) {
        context->preempt_requested = 1U;
    }
}
