#include "pic.h"
#include "io.h"

#include <stdint.h>

/* 8259 PIC port addresses */
#define PIC1_CMD  0x20U
#define PIC1_DATA 0x21U
#define PIC2_CMD  0xA0U
#define PIC2_DATA 0xA1U

/* 8259 control words */
#define PIC_EOI      0x20U   /* End-of-interrupt command */
#define ICW1_ICW4    0x01U   /* ICW4 present              */
#define ICW1_INIT    0x10U   /* Initialization command    */
#define ICW4_8086    0x01U   /* 8086/88 (not MCS-80) mode */

/* Writing to port 0x80 is a well-known way to introduce a small I/O delay
 * that lets old ISA peripherals (including the 8259) keep up. */
static void io_wait(void)
{
    outb(0x80, 0x00);
}

void pic_remap(uint8_t offset1, uint8_t offset2)
{
    /* Save current interrupt masks so they can be restored after remapping. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start initialisation sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: set vector offsets */
    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    /* ICW3: tell master that slave is on IRQ2; tell slave its cascade id */
    outb(PIC1_DATA, 4); io_wait(); /* master: slave on IR2 (bit mask) */
    outb(PIC2_DATA, 2); io_wait(); /* slave:  cascade identity = 2     */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Restore masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8U) {
        outb(PIC2_CMD, (uint8_t)PIC_EOI);
    }
    outb(PIC1_CMD, (uint8_t)PIC_EOI);
}

void pic_mask_irq(uint8_t irq)
{
    if (irq < 8U) {
        uint8_t mask = (uint8_t)(inb(PIC1_DATA) | (uint8_t)(1U << irq));
        outb(PIC1_DATA, mask);
    } else if (irq < 16U) {
        uint8_t slave_mask = (uint8_t)(inb(PIC2_DATA)
            | (uint8_t)(1U << (irq - 8U)));
        outb(PIC2_DATA, slave_mask);
        if (slave_mask == 0xFFU) {
            uint8_t master_mask = (uint8_t)(inb(PIC1_DATA) | (1U << 2));
            outb(PIC1_DATA, master_mask);
        }
    }
}

void pic_unmask_irq(uint8_t irq)
{
    if (irq < 8U) {
        uint8_t mask = (uint8_t)(inb(PIC1_DATA)
            & (uint8_t)~(1U << irq));
        outb(PIC1_DATA, mask);
    } else if (irq < 16U) {
        uint8_t slave_mask = (uint8_t)(inb(PIC2_DATA)
            & (uint8_t)~(1U << (irq - 8U)));
        uint8_t master_mask = (uint8_t)(inb(PIC1_DATA)
            & (uint8_t)~(1U << 2));
        outb(PIC2_DATA, slave_mask);
        outb(PIC1_DATA, master_mask);
    }
}

void pic_disable_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
