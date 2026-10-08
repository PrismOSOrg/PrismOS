#include "interrupts.h"
#include "idt.h"
#include "irq.h"
#include "platform/gdt.h"
#include "platform/io.h"
#include "platform/pic.h"
#include "display/console.h"
#include "debug/log.h"

#define PIT_CHANNEL0 0x40U
#define PIT_COMMAND 0x43U
#define PIT_BASE_FREQUENCY 1193182U
#define PIT_FREQUENCY 100U

static volatile uint32_t uptime_milliseconds;

static void pit_irq_handler(registers_t* regs)
{
    (void)regs;
    uptime_milliseconds += 1000U / PIT_FREQUENCY;
    console_tick();
}

uint32_t system_uptime_ms(void)
{
    return uptime_milliseconds;
}

static void pit_init(void)
{
    const uint16_t divisor = (uint16_t)(PIT_BASE_FREQUENCY / PIT_FREQUENCY);

    outb(PIT_COMMAND, 0x36U);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

void interrupts_init(void)
{
    DEBUG_LOG("interrupts: installing GDT");
    gdt_init();

    DEBUG_LOG("interrupts: remapping PIC");
    /* IRQ0–7  → vectors 0x20–0x27
     * IRQ8–15 → vectors 0x28–0x2F
     * This moves hardware IRQs above the CPU exception range (0x00–0x1F). */
    pic_remap(0x20, 0x28);

    /* Mask every IRQ line so only drivers that explicitly unmask their line
     * will receive interrupts.  This prevents spurious IRQs from firing
     * before any handler is registered. */
    pic_disable_all();

    DEBUG_LOG("interrupts: installing IDT");
    idt_init();

    DEBUG_LOG("interrupts: initialising IRQ handler table");
    irq_init();

    pit_init();
    irq_register_handler(0, pit_irq_handler);
    pic_unmask_irq(0);

    DEBUG_LOG("interrupts: enabling interrupts");
    __asm__ volatile("sti");
    DEBUG_LOG("interrupts_init: complete - system ready for interrupts");
}
