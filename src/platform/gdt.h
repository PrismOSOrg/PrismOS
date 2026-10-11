#ifndef PRISMOS_GDT_H
#define PRISMOS_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08U
#define GDT_KERNEL_DATA_SELECTOR 0x10U
#define GDT_USER_CODE_SELECTOR 0x1BU
#define GDT_USER_DATA_SELECTOR 0x23U

/* Initialise flat kernel/user segments and a 32-bit TSS before idt_init(). */
void gdt_init(void);
void gdt_set_kernel_stack(uint32_t stack_top);
uint32_t gdt_get_kernel_stack(void);

#endif /* PRISMOS_GDT_H */
