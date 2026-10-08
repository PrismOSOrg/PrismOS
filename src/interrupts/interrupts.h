#ifndef PRISMOS_INTERRUPTS_H
#define PRISMOS_INTERRUPTS_H

#include <stdint.h>

/* Single entry-point that brings up the full interrupt subsystem:
 *   1. Install own GDT (flat 32-bit code/data)
 *   2. Remap 8259 PIC (IRQ0–7 → 0x20, IRQ8–15 → 0x28)
 *   3. Mask all IRQ lines
 *   4. Install IDT with stubs for CPU exceptions + IRQ vectors
 *   5. Initialise the IRQ handler table
 *   6. Enable interrupts (sti)
 *
 * After this call, drivers register themselves with:
 *   irq_register_handler(irq_line, my_handler);
 *   pic_unmask_irq(irq_line);
 */
void interrupts_init(void);

/* Monotonic uptime based on the 100 Hz PIT, in milliseconds. */
uint32_t system_uptime_ms(void);

#endif /* PRISMOS_INTERRUPTS_H */
