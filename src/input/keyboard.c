#include "keyboard.h"

#include "comport/comport.h"
#include "interrupts/irq.h"
#include "platform/io.h"
#include "platform/pic.h"
#include "debug/log.h"

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* -------------------------------------------------------------------------
 * KeyEvent ring buffer — written by the IRQ handler, read by the shell.
 * ---------------------------------------------------------------------- */
#define KB_QUEUE_SIZE 64U

typedef struct {
    KeyEvent buf[KB_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
} KeyEventQueue;

static KeyEventQueue kb_queue;

static int kb_queue_empty(void)  { return kb_queue.head == kb_queue.tail; }
static int kb_queue_full(void)   { return ((kb_queue.head + 1U) % KB_QUEUE_SIZE) == kb_queue.tail; }

static void kb_queue_push(KeyEvent ev)
{
    if (!kb_queue_full()) {
        kb_queue.buf[kb_queue.head] = ev;
        kb_queue.head = (kb_queue.head + 1U) % KB_QUEUE_SIZE;
    }
}

static KeyEvent kb_queue_pop(void)
{
    KeyEvent ev = kb_queue.buf[kb_queue.tail];
    kb_queue.tail = (kb_queue.tail + 1U) % KB_QUEUE_SIZE;
    return ev;
}

/* -------------------------------------------------------------------------
 * Scancode translation tables
 * ---------------------------------------------------------------------- */
static const char keyboard_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0,
    ' ', 0
};

static const char keyboard_shift_map[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0,
    ' ', 0
};

static int keyboard_data_available(void) {
    return (inb(KEYBOARD_STATUS_PORT) & 1) != 0;
}

/* -------------------------------------------------------------------------
 * IRQ1 handler — runs with interrupts disabled.
 * Reads one scancode, converts it to a KeyEvent, queues it.
 * ---------------------------------------------------------------------- */
static void keyboard_irq_handler(registers_t *regs)
{
    (void)regs;

    static int extended_prefix = 0;
    static int shift_pressed   = 0;
    static int ctrl_pressed    = 0;
    static int caps_lock       = 0;
    static int first_invocation = 1;

    if (first_invocation) {
        DEBUG_LOG("keyboard_irq_handler: first interrupt on IRQ1");
        first_invocation = 0;
    }

    /* Read the scancode only if data is actually available (guards against
     * spurious invocations). */
    if (!keyboard_data_available()) {
        return;
    }

    unsigned char scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode == 0xE0) {
        extended_prefix = 1;
        return;
    }

    /* Key-release events */
    if (scancode & 0x80) {
        unsigned char key = (unsigned char)(scancode & 0x7F);
        if (key == 42 || key == 54) { /* left/right shift */
            shift_pressed = 0;
        }
        if (key == 29) { /* left/right control */
            ctrl_pressed = 0;
        }
        extended_prefix = 0;
        return;
    }

    if (extended_prefix) {
        extended_prefix = 0;
        KeyEvent ev = {KEY_EVENT_NONE, 0, 0};
        ev.modifiers = (uint8_t)((shift_pressed ? KEY_MOD_SHIFT : 0U)
            | (ctrl_pressed ? KEY_MOD_CTRL : 0U));
        switch (scancode) {
            case 0x48: ev.type = KEY_EVENT_UP;     break;
            case 0x50: ev.type = KEY_EVENT_DOWN;   break;
            case 0x4B: ev.type = KEY_EVENT_LEFT;   break;
            case 0x4D: ev.type = KEY_EVENT_RIGHT;  break;
            case 0x47: ev.type = KEY_EVENT_HOME;   break;
            case 0x4F: ev.type = KEY_EVENT_END;    break;
            case 0x53: ev.type = KEY_EVENT_DELETE; break;
            default:   return;
        }
        kb_queue_push(ev);
        return;
    }

    if (scancode == 42 || scancode == 54) { shift_pressed = 1; return; }
    if (scancode == 29) { ctrl_pressed = 1; return; }
    if (scancode == 58) { caps_lock = !caps_lock; return; }
    if (scancode == 14) { kb_queue_push((KeyEvent){KEY_EVENT_BACKSPACE, 0, 0}); return; }
    if (scancode == 28) { kb_queue_push((KeyEvent){KEY_EVENT_ENTER, 0, 0}); return; }
    if (scancode >= 128 || keyboard_map[scancode] == 0) { return; }

    char character = shift_pressed ? keyboard_shift_map[scancode] : keyboard_map[scancode];

    if (ctrl_pressed && (character == 'c' || character == 'C')) {
        kb_queue_push((KeyEvent){KEY_EVENT_COPY, 0, KEY_MOD_CTRL});
        return;
    }

    if (ctrl_pressed && (character == 'v' || character == 'V')) {
        kb_queue_push((KeyEvent){KEY_EVENT_PASTE, 0, KEY_MOD_CTRL});
        return;
    }
    if (caps_lock && character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    } else if (caps_lock && character >= 'A' && character <= 'Z') {
        character = (char)(character - 'A' + 'a');
    }

    kb_queue_push((KeyEvent){KEY_EVENT_CHARACTER, character,
        (uint8_t)((shift_pressed ? KEY_MOD_SHIFT : 0U)
            | (ctrl_pressed ? KEY_MOD_CTRL : 0U))});
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
void keyboard_init(void)
{
    kb_queue.head = 0;
    kb_queue.tail = 0;

    irq_register_handler(1, keyboard_irq_handler);
    pic_unmask_irq(1);
}

static KeyEvent keyboard_serial_event(int character) {
    if (character == '\r' || character == '\n') {
        return (KeyEvent){KEY_EVENT_ENTER, 0, 0};
    }

    if (character == '\b' || character == 0x7FU) {
        return (KeyEvent){KEY_EVENT_BACKSPACE, 0, 0};
    }

    return (KeyEvent){KEY_EVENT_CHARACTER, (char)character, 0};
}

KeyEvent keyboard_read_event(void)
{
    /* Sleep with 'hlt' until the IRQ handler deposits an event. */
    while (kb_queue_empty()) {
        int serial_character = comport_poll_char();
        if (serial_character >= 0) {
            return keyboard_serial_event(serial_character);
        }
        __asm__ volatile("hlt");
    }
    return kb_queue_pop();
}

KeyEvent keyboard_poll_event(void)
{
    if (kb_queue_empty()) {
        int serial_character = comport_poll_char();
        if (serial_character >= 0) {
            return keyboard_serial_event(serial_character);
        }

        return (KeyEvent){KEY_EVENT_NONE, 0, 0};
    }

    return kb_queue_pop();
}