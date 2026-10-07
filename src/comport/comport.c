#include "comport.h"

#include <stdint.h>

#include "platform/io.h"
#include "platform/pic.h"
#include "interrupts/irq.h"
#include "util/ringbuf.h"
#include "debug/log.h"

#define PORT 0x3f8

/* Ring buffer for received bytes, filled by the IRQ4 handler. */
static ringbuf_t rx_buf;

int comport_init(void) {
    ringbuf_init(&rx_buf);

    outb(PORT + 1, 0x00);    // Disable interrupts during init
    outb(PORT + 3, 0x80);    // Enable DLAB

    outb(PORT + 0, 0x03);    // Baud divisor low (38400)
    outb(PORT + 1, 0x00);    // Baud divisor high

    outb(PORT + 3, 0x03);    // 8 bits, no parity, 1 stop bit
    outb(PORT + 2, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set

    // Loopback test
    outb(PORT + 4, 0x1E);    // Enable loopback mode
    outb(PORT + 0, 0xAE);    // Send test byte

    if (inb(PORT + 0) != 0xAE) {
        return 1; // failed
    }

    // Normal operation
    outb(PORT + 4, 0x0F);

    /* Enable "Received Data Available" interrupt (IER bit 0).
     * The IRQ4 handler will push incoming bytes into rx_buf.
     * irq_register_handler / pic_unmask_irq are called from comport_irq_init()
     * so that the caller controls when the IRQ line is actually unmasked. */
    outb(PORT + 1, 0x01);

    return 0;
}

/* -------------------------------------------------------------------------
 * IRQ4 (COM1) receive handler — called from irq_dispatcher with IF=0.
 * Drains the UART FIFO into rx_buf.
 * ---------------------------------------------------------------------- */
static void comport_irq_handler(registers_t *regs)
{
    (void)regs;
    static int first_invocation = 1;

    if (first_invocation) {
        DEBUG_LOG("comport_irq_handler: first interrupt on IRQ4");
        first_invocation = 0;
    }

    /* Read all available bytes from the FIFO */
    while (inb(PORT + 5) & 0x01) {
        ringbuf_push(&rx_buf, inb(PORT));
    }
}

/* Register the COM1 IRQ handler and unmask IRQ4 on the PIC.
 * Must be called after interrupts_init(). */
void comport_irq_init(void)
{
    irq_register_handler(4, comport_irq_handler);
    pic_unmask_irq(4);
}

// -------------------------
// Status helpers
// -------------------------
static int serial_transmit_empty() {
    return inb(PORT + 5) & 0x20;
}

// -------------------------
// Read / Write single byte
// -------------------------
static char serial_read(void) {
    /* Block with hlt-loop, sleeping until the IRQ handler delivers a byte. */
    while (ringbuf_empty(&rx_buf)) {
        __asm__ volatile("hlt");
    }
    return (char)ringbuf_pop(&rx_buf);
}

int comport_poll_char(void) {
    if (ringbuf_empty(&rx_buf)) {
        return -1;
    }

    return (unsigned char)ringbuf_pop(&rx_buf);
}

void comport_write_char(char c) {
    while (!serial_transmit_empty());
    outb(PORT, c);
}

// -------------------------
// Write string
// -------------------------
void comport_write_string(const char* str) {
    while (*str) {
        comport_write_char(*str++);
    }
}

// -------------------------
// Read buffer (blocking, fixed length)
// -------------------------
void comport_read_buffer(char* buffer, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        buffer[i] = serial_read();
    }
}