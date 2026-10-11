#include "irq.h"
#include "platform/pic.h"
#include "debug/log.h"
#include "display/console.h"
#include "platform/user_mode.h"
#include "drivers/driver_api.h"

#include <stddef.h>

/* 16 slots — one per hardware IRQ line on the 8259 PIC */
#define IRQ_COUNT 16U
#define EXCEPTION_COUNT 32U

static irq_handler_t irq_handlers[IRQ_COUNT];
static exception_handler_t exception_handlers[EXCEPTION_COUNT];
static uint8_t driver_irq_references[IRQ_COUNT];

static void console_write_hex32(uint32_t value) {
    char text[9];

    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (uint8_t)(value & 0x0FU);
        text[i] = (char)((nibble < 10U) ? ('0' + (char)nibble) : ('A' + (char)(nibble - 10U)));
        value >>= 4;
    }

    text[8] = '\0';
    console_write(text);
}

static const char* exception_name(uint8_t vector) {
    switch (vector) {
        case 0: return "Divide by zero";
        case 1: return "Debug";
        case 2: return "Non-maskable interrupt";
        case 3: return "Breakpoint";
        case 4: return "Overflow";
        case 5: return "Bound range exceeded";
        case 6: return "Invalid opcode";
        case 7: return "Device not available";
        case 8: return "Double fault";
        case 9: return "Coprocessor segment overrun";
        case 10: return "Invalid TSS";
        case 11: return "Segment not present";
        case 12: return "Stack fault";
        case 13: return "General protection fault";
        case 14: return "Page fault";
        case 15: return "Reserved";
        case 16: return "x87 floating point";
        case 17: return "Alignment check";
        case 18: return "Machine check";
        case 19: return "SIMD floating point";
        case 20: return "Virtualization";
        default: return "CPU exception";
    }
}

static void panic_screen(registers_t* regs) {
    console_set_color(COLOR_WHITE, COLOR_RED);
    console_clear();
    console_set_cursor(0, 0);
    console_writeln("*** PRISMOS PANIC ***");
    console_writeln("An unhandled CPU exception occurred.");
    console_write("Vector: ");
    console_write_uint((unsigned int)regs->int_no);
    console_writeln("");
    console_write("Name: ");
    console_writeln(exception_name((uint8_t)regs->int_no));
    console_write("Error code: ");
    console_write_uint((unsigned int)regs->err_code);
    console_writeln("");
    console_write("EIP: 0x");
    console_write_hex32(regs->eip);
    console_writeln("");
    console_write("CS: 0x");
    console_write_hex32(regs->cs);
    console_writeln("");
    console_write("EFLAGS: 0x");
    console_write_hex32(regs->eflags);
    console_writeln("");
    console_writeln("System halted.");

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void irq_init(void)
{
    DEBUG_LOG("irq_init: initializing handler table");
    for (unsigned i = 0; i < IRQ_COUNT; i++) {
        irq_handlers[i] = NULL;
        driver_irq_references[i] = 0U;
    }

    for (unsigned i = 0; i < EXCEPTION_COUNT; i++) {
        exception_handlers[i] = NULL;
    }
}

void irq_register_handler(uint8_t irq, irq_handler_t handler)
{
    if (irq < IRQ_COUNT) {
        DEBUG_LOG("irq_register_handler: registered handler");
        irq_handlers[irq] = handler;
    }
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq < IRQ_COUNT) {
        DEBUG_LOG("irq_unregister_handler: unregistered handler");
        irq_handlers[irq] = NULL;
    }
}

int irq_driver_line_acquire(uint8_t irq)
{
    if (irq >= IRQ_COUNT || irq == 0U || irq == 1U || irq == 2U
        || irq == 4U || irq == 8U || irq_handlers[irq] != NULL
        || driver_irq_references[irq] != 0U) {
        return -1;
    }

    driver_irq_references[irq] = 1U;
    pic_unmask_irq(irq);
    return 0;
}

void irq_driver_line_release(uint8_t irq)
{
    if (irq >= IRQ_COUNT || driver_irq_references[irq] == 0U) {
        return;
    }

    if (--driver_irq_references[irq] == 0U && irq_handlers[irq] == NULL) {
        pic_mask_irq(irq);
    }
}

void irq_driver_line_mask(uint8_t irq)
{
    if (irq < IRQ_COUNT && driver_irq_references[irq] != 0U) {
        pic_mask_irq(irq);
    }
}

void irq_driver_line_ack(uint8_t irq)
{
    if (irq < IRQ_COUNT && driver_irq_references[irq] != 0U) {
        pic_unmask_irq(irq);
    }
}

void exception_register_handler(uint8_t vector, exception_handler_t handler)
{
    if (vector < EXCEPTION_COUNT) {
        exception_handlers[vector] = handler;
    }
}

void exception_unregister_handler(uint8_t vector)
{
    if (vector < EXCEPTION_COUNT) {
        exception_handlers[vector] = NULL;
    }
}

/* Called from irq_common_stub in isr_stubs.s.
 * int_no is 32 + IRQ line, so subtract 32 to get the IRQ index. */
void irq_dispatcher(registers_t *regs)
{
    uint8_t irq = (uint8_t)(regs->int_no - 32U);

    if (irq < IRQ_COUNT && irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
    if (irq < IRQ_COUNT) {
        driver_runtime_notify_irq(irq);
    }

    /* Always acknowledge the PIC — even for unhandled IRQs */
    pic_send_eoi(irq);
}

/* Called from isr_common_stub for CPU exceptions (vectors 0–31).
 * Minimal handler: log and halt.  Drivers can hook specific exceptions
 * by adding a dispatch table here later (e.g. for #PF, #BP). */
void isr_handler(registers_t *regs)
{
    if ((regs->cs & 3U) == 3U && user_mode_handle_exception(regs)) {
        return;
    }

    if (regs->int_no < EXCEPTION_COUNT && exception_handlers[regs->int_no] != NULL) {
        exception_handlers[regs->int_no](regs);
        return;
    }

    ERROR_LOG("unhandled CPU exception");
    /* TODO: per-exception dispatch table for page-fault handler, etc. */
    panic_screen(regs);
}
