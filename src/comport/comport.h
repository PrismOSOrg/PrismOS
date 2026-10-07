#ifndef PRISMOS_COMPORT_H
#define PRISMOS_COMPORT_H

#include <stdint.h>

int  comport_init(void);

/* Register the COM1 IRQ4 handler and unmask IRQ4.
 * Call after interrupts_init() and comport_init(). */
void comport_irq_init(void);

void comport_write_char(char c);
void comport_write_string(const char* str);
void comport_read_buffer(char* buffer, uint32_t len);
int comport_poll_char(void);

#endif