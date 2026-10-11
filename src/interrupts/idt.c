#include "idt.h"
#include "irq.h"
#include "debug/log.h"
#include "platform/gdt.h"

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Forward declarations — defined in isr_stubs.s
 * ---------------------------------------------------------------------- */
/* CPU exception stubs */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* Hardware IRQ stubs (vectors 0x20–0x2F) */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);
extern void isr128(void);

/* -------------------------------------------------------------------------
 * IDT storage
 * ---------------------------------------------------------------------- */
static IdtEntry idt[256];
static IdtPtr   idtr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_low  = (uint16_t)(base & 0xFFFFU);
    idt[num].base_high = (uint16_t)((base >> 16) & 0xFFFFU);
    idt[num].selector  = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

void idt_init(void)
{
    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base  = (uint32_t)(uintptr_t)idt;

    /* Clear all entries first */
    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    /* 0x8E = interrupt gate, present, DPL=0 */
#define GATE(n, fn) idt_set_gate((n), (uint32_t)(fn), 0x08, 0x8E)

    /* CPU exceptions 0–31 */
    GATE( 0, isr0);  GATE( 1, isr1);  GATE( 2, isr2);  GATE( 3, isr3);
    GATE( 4, isr4);  GATE( 5, isr5);  GATE( 6, isr6);  GATE( 7, isr7);
    GATE( 8, isr8);  GATE( 9, isr9);  GATE(10, isr10); GATE(11, isr11);
    GATE(12, isr12); GATE(13, isr13); GATE(14, isr14); GATE(15, isr15);
    GATE(16, isr16); GATE(17, isr17); GATE(18, isr18); GATE(19, isr19);
    GATE(20, isr20); GATE(21, isr21); GATE(22, isr22); GATE(23, isr23);
    GATE(24, isr24); GATE(25, isr25); GATE(26, isr26); GATE(27, isr27);
    GATE(28, isr28); GATE(29, isr29); GATE(30, isr30); GATE(31, isr31);

    /* Hardware IRQs 0–15 → vectors 0x20–0x2F */
    GATE(32, irq0);  GATE(33, irq1);  GATE(34, irq2);  GATE(35, irq3);
    GATE(36, irq4);  GATE(37, irq5);  GATE(38, irq6);  GATE(39, irq7);
    GATE(40, irq8);  GATE(41, irq9);  GATE(42, irq10); GATE(43, irq11);
    GATE(44, irq12); GATE(45, irq13); GATE(46, irq14); GATE(47, irq15);

#undef GATE

    idt_set_gate(0x80U, (uint32_t)(uintptr_t)isr128,
        GDT_KERNEL_CODE_SELECTOR, 0xEEU);

    __asm__ volatile("lidt (%0)" : : "r"(&idtr) : "memory");
    DEBUG_LOG("idt_init: installed IDT with 32 exceptions + 16 IRQs");
}
