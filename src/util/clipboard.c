#include "clipboard.h"

#include "comport/comport.h"

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char clipboard[PRISMOS_CLIPBOARD_CAPACITY + 1U];
static uint32_t clipboard_size;

void clipboard_clear(void) {
    clipboard_size = 0U;
    clipboard[0] = '\0';
}

void clipboard_set(const char* text, uint32_t length) {
    if (text == 0) {
        clipboard_clear();
        return;
    }

    if (length > PRISMOS_CLIPBOARD_CAPACITY) {
        length = PRISMOS_CLIPBOARD_CAPACITY;
    }

    for (uint32_t i = 0; i < length; i++) {
        clipboard[i] = text[i];
    }

    clipboard_size = length;
    clipboard[clipboard_size] = '\0';

    /* OSC 52 lets a serial terminal expose the guest selection to the host. */
    comport_write_string("\033]52;c;");
    for (uint32_t i = 0; i < clipboard_size; i += 3U) {
        uint32_t value = (uint32_t)(uint8_t)clipboard[i] << 16;
        uint32_t remaining = clipboard_size - i;

        if (remaining > 1U) {
            value |= (uint32_t)(uint8_t)clipboard[i + 1U] << 8;
        }
        if (remaining > 2U) {
            value |= (uint32_t)(uint8_t)clipboard[i + 2U];
        }

        comport_write_char(base64_alphabet[(value >> 18) & 0x3FU]);
        comport_write_char(base64_alphabet[(value >> 12) & 0x3FU]);
        comport_write_char(remaining > 1U ? base64_alphabet[(value >> 6) & 0x3FU] : '=');
        comport_write_char(remaining > 2U ? base64_alphabet[value & 0x3FU] : '=');
    }
    comport_write_string("\007");
}

uint32_t clipboard_length(void) {
    return clipboard_size;
}

const char* clipboard_data(void) {
    return clipboard;
}
