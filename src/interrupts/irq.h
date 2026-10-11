#ifndef PRISMOS_IRQ_H
#define PRISMOS_IRQ_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * CPU context pushed by the ISR/IRQ stubs onto the stack.
 *
 * Stack layout (top = low address) when the C handler is called:
 *   ds           <- pushed last by stub
 *   edi..eax     <- pusha  (edi,esi,ebp,esp_dummy,ebx,edx,ecx,eax)
 *   int_no       <- pushed by stub
 *   err_code     <- pushed by stub (or by CPU for certain exceptions)
 *   eip,cs,eflags<- pushed by CPU on interrupt entry
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pusha order */
    uint32_t int_no;    /* 0–31 = CPU exception, 32–47 = IRQ 0–15          */
    uint32_t err_code;
    uint32_t eip, cs, eflags; /* CPU frame prefix; user ESP/SS follow on ring changes */
} registers_t;

/* Callback signature for all registered interrupt handlers. */
typedef void (*irq_handler_t)(registers_t *regs);
typedef void (*exception_handler_t)(registers_t *regs);

/* -------------------------------------------------------------------------
 * Initialise the IRQ handler table (all slots empty).
 * Called once from interrupts_init(). ---------------------------------------------------------------------- */
void irq_init(void);

/* Register / unregister a handler for IRQ line 0–15.
 * Adding a new device (mouse, disk, etc.) only requires calling these two
 * functions — no changes anywhere else are needed. */
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unregister_handler(uint8_t irq);
int irq_driver_line_acquire(uint8_t irq);
void irq_driver_line_release(uint8_t irq);
void irq_driver_line_mask(uint8_t irq);
void irq_driver_line_ack(uint8_t irq);

/* Register / unregister a CPU exception handler for vectors 0-31. */
void exception_register_handler(uint8_t vector, exception_handler_t handler);
void exception_unregister_handler(uint8_t vector);

/* -------------------------------------------------------------------------
 * Called from isr_stubs.s — do not call directly.
 * ---------------------------------------------------------------------- */
void irq_dispatcher(registers_t *regs);   /* for IRQ vectors  0x20–0x2F */
void isr_handler(registers_t *regs);      /* for CPU exception vectors  */

#endif /* PRISMOS_IRQ_H */
