#include "gdt.h"

#include <stdint.h>

/* -------------------------------------------------------------------------
 * GDT entry / pointer types
 * ---------------------------------------------------------------------- */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity; /* [7:4] flags | [3:0] limit[19:16] */
    uint8_t  base_high;
} __attribute__((packed)) GdtEntry;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) GdtPtr;

typedef struct {
    uint32_t previous_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) GdtTss;

#define GDT_KERNEL_TSS_SELECTOR 0x28U
#define GDT_ENTRY_COUNT 6

static GdtEntry gdt[GDT_ENTRY_COUNT];
static GdtPtr   gdtr;
static GdtTss tss;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran_flags)
{
    gdt[idx].base_low   = (uint16_t)(base  & 0xFFFFU);
    gdt[idx].base_mid   = (uint8_t)((base  >> 16) & 0xFFU);
    gdt[idx].base_high  = (uint8_t)((base  >> 24) & 0xFFU);
    gdt[idx].limit_low  = (uint16_t)(limit & 0xFFFFU);
    /* upper nibble of granularity = flags, lower nibble = limit[19:16] */
    gdt[idx].granularity = (uint8_t)(((limit >> 16) & 0x0FU) | (gran_flags & 0xF0U));
    gdt[idx].access = access;
}

void gdt_init(void)
{
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint32_t)(uintptr_t)gdt;

    gdt_set_entry(0, 0,          0,           0x00, 0x00); /* Null          */
    gdt_set_entry(1, 0, 0xFFFFFFFFU, 0x9A, 0xCF); /* Kernel code   */
    gdt_set_entry(2, 0, 0xFFFFFFFFU, 0x92, 0xCF); /* Kernel data   */
    gdt_set_entry(3, 0, 0xFFFFFFFFU, 0xFA, 0xCF); /* User code     */
    gdt_set_entry(4, 0, 0xFFFFFFFFU, 0xF2, 0xCF); /* User data     */

    tss = (GdtTss){0};
    tss.ss0 = GDT_KERNEL_DATA_SELECTOR;
    tss.iomap_base = (uint16_t)sizeof(tss);
    gdt_set_entry(5, (uint32_t)(uintptr_t)&tss, sizeof(tss) - 1U, 0x89, 0x00);

    /* Load the new GDT and reload all segment registers.
     * The far-return idiom (push cs + eip, then lret) is used to reload %cs
     * because a direct ljmp is not easily expressible in inline AT&T asm. */
    __asm__ volatile(
        "lgdt (%0)          \n\t"
        "movw $0x10, %%ax   \n\t"
        "movw %%ax,  %%ds   \n\t"
        "movw %%ax,  %%es   \n\t"
        "movw %%ax,  %%fs   \n\t"
        "movw %%ax,  %%gs   \n\t"
        "movw %%ax,  %%ss   \n\t"
        "pushl $0x08        \n\t"   /* new CS */
        "pushl $1f          \n\t"   /* new EIP */
        "lret               \n\t"
        "1:                 \n\t"
        :
        : "r"(&gdtr)
        : "eax", "memory"
    );

    {
        uint16_t tss_selector = GDT_KERNEL_TSS_SELECTOR;
        __asm__ volatile ("ltr %0" : : "rm"(tss_selector) : "memory");
    }
}

void gdt_set_kernel_stack(uint32_t stack_top)
{
    tss.esp0 = stack_top;
}

uint32_t gdt_get_kernel_stack(void)
{
    return tss.esp0;
}
