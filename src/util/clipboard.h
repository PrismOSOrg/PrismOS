#ifndef PRISMOS_CLIPBOARD_H
#define PRISMOS_CLIPBOARD_H

#include <stdint.h>

#define PRISMOS_CLIPBOARD_CAPACITY (16U * 1024U)

void clipboard_clear(void);
void clipboard_set(const char* text, uint32_t length);
uint32_t clipboard_length(void);
const char* clipboard_data(void);

#endif
