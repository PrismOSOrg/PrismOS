#include "apps/bytecode_vm.h"

#include "apps/app_format.h"
#include "apps/app_manager.h"
#include "apps/user_driver_vm.h"
#include "debug/log.h"
#include "display/console.h"
#include "drivers/driver_api.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"
#include "commands/command.h"

#define BCVM_STACK_MAX 256U
#define BCVM_LOCALS_MAX 512U
#define BCVM_CALL_DEPTH_MAX 32U
#define BCVM_MAX_STEPS 200000U
#define BCVM_INPUT_MAX 127U
#define BCVM_HEAP_STRING_MAX 64U
#define BCVM_HEAP_STRING_BYTES 4096U
#define BCVM_HEAP_ARRAY_MAX 64U
#define BCVM_HEAP_ARRAY_CELLS 1024U
#define BCVM_FILE_PATH_MAX 128U
#define BCVM_FILE_TEXT_MAX 2048U
#define BCVM_FLOAT_SCALE 1000

typedef struct {
    char input_buffer[BCVM_INPUT_MAX + 1U];
    uint16_t input_length;
    char heap_string_data[BCVM_HEAP_STRING_BYTES];
    uint16_t heap_string_offset[BCVM_HEAP_STRING_MAX];
    uint16_t heap_string_length[BCVM_HEAP_STRING_MAX];
    uint8_t heap_string_used[BCVM_HEAP_STRING_MAX];
    uint16_t heap_next_offset;
    uint8_t heap_next_slot;
    int32_t heap_array_data[BCVM_HEAP_ARRAY_CELLS];
    uint16_t heap_array_offset[BCVM_HEAP_ARRAY_MAX];
    uint16_t heap_array_length[BCVM_HEAP_ARRAY_MAX];
    uint8_t heap_array_used[BCVM_HEAP_ARRAY_MAX];
    uint16_t heap_array_next_offset;
    uint8_t heap_array_next_slot;
    int32_t stack[BCVM_STACK_MAX];
    int32_t locals[BCVM_CALL_DEPTH_MAX + 1U][BCVM_LOCALS_MAX];
    uint32_t call_return_pc[BCVM_CALL_DEPTH_MAX];
    char file_path_buffer[BCVM_FILE_PATH_MAX];
    char file_text_buffer[BCVM_FILE_TEXT_MAX + 1U];
    uint32_t pc;
    uint32_t sp;
    uint32_t call_depth;
    uint32_t steps;
    uint32_t entry_offset;
    int32_t return_value;
    uint8_t stop_requested;
    const uint8_t* image;
    uint32_t image_size;
    uint8_t active_call;
} BytecodeVmContext;

static BytecodeVmContext app_vm_context;
static BytecodeVmContext* active_vm_context;

#define vm_input_buffer (active_vm_context->input_buffer)
#define vm_input_length (active_vm_context->input_length)
#define vm_heap_string_data (active_vm_context->heap_string_data)
#define vm_heap_string_offset (active_vm_context->heap_string_offset)
#define vm_heap_string_length (active_vm_context->heap_string_length)
#define vm_heap_string_used (active_vm_context->heap_string_used)
#define vm_heap_next_offset (active_vm_context->heap_next_offset)
#define vm_heap_next_slot (active_vm_context->heap_next_slot)
#define vm_heap_array_data (active_vm_context->heap_array_data)
#define vm_heap_array_offset (active_vm_context->heap_array_offset)
#define vm_heap_array_length (active_vm_context->heap_array_length)
#define vm_heap_array_used (active_vm_context->heap_array_used)
#define vm_heap_array_next_offset (active_vm_context->heap_array_next_offset)
#define vm_heap_array_next_slot (active_vm_context->heap_array_next_slot)
#define vm_stack (active_vm_context->stack)
#define vm_locals (active_vm_context->locals)
#define vm_call_return_pc (active_vm_context->call_return_pc)
#define vm_file_path_buffer (active_vm_context->file_path_buffer)
#define vm_file_text_buffer (active_vm_context->file_text_buffer)

#define VM_STRING_DESC_HEAP_MASK 0x80000000U

static int vm_system_command_allowed(const char* command, uint16_t length) {
    uint16_t first_end = 0;
    const char* allowed[] = {"help", "drive", "partitions", "net", "pwd", "ls"};
    int matched = 0;

    while (first_end < length && command[first_end] != ' ') {
        if (command[first_end] == ';' || command[first_end] == '|'
            || command[first_end] == '&' || command[first_end] == '>'
            || command[first_end] == '<' || command[first_end] == '`') {
            return 0;
        }
        first_end++;
    }

    if (first_end == 0U) {
        return 0;
    }

    for (uint32_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        uint32_t allowed_length = 0;
        while (allowed[i][allowed_length] != '\0') {
            allowed_length++;
        }
        if (allowed_length == first_end) {
            matched = 1;
            for (uint16_t j = 0; j < first_end; j++) {
                if (command[j] != allowed[i][j]) {
                    matched = 0;
                    break;
                }
            }
            if (matched) {
                break;
            }
        }
    }

    if (!matched) {
        return 0;
    }

    for (uint16_t i = first_end; i < length; i++) {
        char c = command[i];
        if (c == ';' || c == '|' || c == '&' || c == '>'
            || c == '<' || c == '`') {
            return 0;
        }
    }
    return 1;
}

static uint32_t vm_make_heap_string_descriptor(uint8_t slot) {
    return VM_STRING_DESC_HEAP_MASK | (uint32_t)slot;
}

static int vm_resolve_string_descriptor(uint32_t descriptor,
    const uint8_t* data,
    const bcvm_image_header_t* header,
    const char** out_ptr,
    uint16_t* out_length) {
    if ((descriptor & VM_STRING_DESC_HEAP_MASK) != 0U) {
        uint8_t slot = (uint8_t)(descriptor & 0xFFU);
        if (slot >= BCVM_HEAP_STRING_MAX || vm_heap_string_used[slot] == 0U) {
            return -1;
        }

        *out_ptr = &vm_heap_string_data[vm_heap_string_offset[slot]];
        *out_length = vm_heap_string_length[slot];
        return 0;
    }

    {
        uint16_t offset = (uint16_t)((descriptor >> 16) & 0x7FFFU);
        uint16_t length = (uint16_t)(descriptor & 0xFFFFU);
        if ((uint32_t)offset + (uint32_t)length > header->data_size) {
            return -1;
        }

        *out_ptr = (const char*)&data[offset];
        *out_length = length;
    }

    return 0;
}

static int vm_store_heap_string(const char* source, uint16_t length, uint32_t* out_descriptor) {
    uint8_t slot;

    if (length > BCVM_HEAP_STRING_BYTES) {
        return -1;
    }

    if (vm_heap_next_offset + length > BCVM_HEAP_STRING_BYTES) {
        vm_heap_next_offset = 0;
        for (uint32_t i = 0; i < BCVM_HEAP_STRING_MAX; i++) {
            vm_heap_string_used[i] = 0U;
        }
    }

    if (vm_heap_next_slot >= BCVM_HEAP_STRING_MAX) {
        vm_heap_next_slot = 0;
    }

    slot = vm_heap_next_slot++;
    vm_heap_string_offset[slot] = vm_heap_next_offset;
    vm_heap_string_length[slot] = length;
    vm_heap_string_used[slot] = 1U;

    for (uint16_t i = 0; i < length; i++) {
        vm_heap_string_data[vm_heap_next_offset + i] = source[i];
    }

    vm_heap_next_offset = (uint16_t)(vm_heap_next_offset + length);
    *out_descriptor = vm_make_heap_string_descriptor(slot);
    return 0;
}

static int vm_copy_string_descriptor_to_buffer(uint32_t descriptor,
    const uint8_t* data,
    const bcvm_image_header_t* header,
    char* out,
    uint32_t out_capacity) {
    const char* ptr;
    uint16_t length;

    if (out == 0 || out_capacity == 0U) {
        return -1;
    }

    if (vm_resolve_string_descriptor(descriptor, data, header, &ptr, &length) != 0) {
        return -1;
    }

    if ((uint32_t)length + 1U > out_capacity) {
        return -1;
    }

    for (uint16_t i = 0; i < length; i++) {
        out[i] = ptr[i];
    }
    out[length] = '\0';
    return 0;
}

static int vm_create_array(uint16_t length, uint32_t* out_handle) {
    uint8_t slot;

    if (length == 0U || length > BCVM_HEAP_ARRAY_CELLS) {
        return -1;
    }

    if ((uint32_t)vm_heap_array_next_offset + length > BCVM_HEAP_ARRAY_CELLS) {
        vm_heap_array_next_offset = 0;
        vm_heap_array_next_slot = 0;
        for (uint32_t i = 0; i < BCVM_HEAP_ARRAY_MAX; i++) {
            vm_heap_array_used[i] = 0U;
        }
    }

    if (vm_heap_array_next_slot >= BCVM_HEAP_ARRAY_MAX) {
        vm_heap_array_next_slot = 0;
    }

    slot = vm_heap_array_next_slot++;
    vm_heap_array_offset[slot] = vm_heap_array_next_offset;
    vm_heap_array_length[slot] = length;
    vm_heap_array_used[slot] = 1U;

    for (uint16_t i = 0; i < length; i++) {
        vm_heap_array_data[vm_heap_array_next_offset + i] = 0;
    }

    vm_heap_array_next_offset = (uint16_t)(vm_heap_array_next_offset + length);
    *out_handle = (uint32_t)slot;
    return 0;
}

static int vm_get_array_value(uint32_t handle, int32_t index, int32_t* out_value) {
    uint16_t offset;
    uint16_t length;

    if (handle >= BCVM_HEAP_ARRAY_MAX || vm_heap_array_used[handle] == 0U || index < 0) {
        return -1;
    }

    offset = vm_heap_array_offset[handle];
    length = vm_heap_array_length[handle];
    if ((uint32_t)index >= length) {
        return -1;
    }

    *out_value = vm_heap_array_data[offset + (uint16_t)index];
    return 0;
}

static int vm_set_array_value(uint32_t handle, int32_t index, int32_t value) {
    uint16_t offset;
    uint16_t length;

    if (handle >= BCVM_HEAP_ARRAY_MAX || vm_heap_array_used[handle] == 0U || index < 0) {
        return -1;
    }

    offset = vm_heap_array_offset[handle];
    length = vm_heap_array_length[handle];
    if ((uint32_t)index >= length) {
        return -1;
    }

    vm_heap_array_data[offset + (uint16_t)index] = value;
    return 0;
}

static uint16_t read_u16le(const uint8_t* source) {
    return (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
}

static uint32_t read_u32le(const uint8_t* source) {
    return (uint32_t)source[0]
        | ((uint32_t)source[1] << 8)
        | ((uint32_t)source[2] << 16)
        | ((uint32_t)source[3] << 24);
}

static int32_t read_i32le(const uint8_t* source) {
    return (int32_t)read_u32le(source);
}

static void vm_write_int(int32_t value) {
    if (value < 0) {
        console_write_char('-');
        console_write_uint((unsigned int)(-value));
        return;
    }

    console_write_uint((unsigned int)value);
}

static void vm_write_float(int32_t value) {
    int32_t abs_value;
    unsigned int integer_part;
    unsigned int fraction_part;

    if (value < 0) {
        console_write_char('-');
    }

    if (value < 0) {
        if ((uint32_t)value == 0x80000000U) {
            abs_value = 2147483647;
        } else {
            abs_value = -value;
        }
    } else {
        abs_value = value;
    }

    integer_part = (unsigned int)(abs_value / BCVM_FLOAT_SCALE);
    fraction_part = (unsigned int)(abs_value % BCVM_FLOAT_SCALE);

    console_write_uint(integer_part);
    console_write_char('.');
    if (fraction_part < 100U) {
        console_write_char('0');
    }
    if (fraction_part < 10U) {
        console_write_char('0');
    }
    console_write_uint(fraction_part);
}

static void vm_erase_last_echoed_char(void) {
    int x = console_get_x();
    int y = console_get_y();

    if (x <= 0) {
        return;
    }

    console_set_cursor(x - 1, y);
    console_write_char(' ');
    console_set_cursor(x - 1, y);
}

static int vm_event_requests_quit(KeyEvent event) {
    return event.type == KEY_EVENT_CHARACTER && event.character == 27;
}

static int vm_poll_quit_request(void) {
    KeyEvent event = keyboard_poll_event();
    if (event.type == KEY_EVENT_NONE) {
        return 0;
    }

    return vm_event_requests_quit(event);
}

static int vm_wait_key_release(char character) {
    while (keyboard_key_pressed(character)) {
        if (keyboard_key_pressed(27)) {
            return 0;
        }
        __asm__ volatile("hlt");
        app_manager_driver_tick();
    }
    return keyboard_key_pressed(27) ? 0 : 1;
}

static int32_t vm_read_int(void) {
    char buffer[32];
    uint32_t length = 0;

    while (1) {
        KeyEvent event = keyboard_read_event();

        if (event.type == KEY_EVENT_ENTER) {
            int32_t value = 0;
            int sign = 1;
            uint32_t i = 0;

            console_write_char('\n');

            if (length == 0U) {
                return 0;
            }

            if (buffer[0] == '-') {
                sign = -1;
                i = 1;
            }

            for (; i < length; i++) {
                value = value * 10 + (int32_t)(buffer[i] - '0');
            }

            return value * sign;
        }

        if (event.type == KEY_EVENT_BACKSPACE) {
            if (length > 0U) {
                length--;
                vm_erase_last_echoed_char();
            }
            continue;
        }

        if (event.type != KEY_EVENT_CHARACTER) {
            continue;
        }

        if (event.character == '-' && length == 0U) {
            buffer[length++] = event.character;
            console_write_char(event.character);
            continue;
        }

        if (event.character >= '0' && event.character <= '9') {
            if (length + 1U < sizeof(buffer)) {
                buffer[length++] = event.character;
                console_write_char(event.character);
            }
        }
    }
}

static int vm_read_text(uint32_t* out_descriptor) {
    vm_input_length = 0;
    vm_input_buffer[0] = '\0';

    while (1) {
        KeyEvent event = keyboard_read_event();

        if (event.type == KEY_EVENT_ENTER) {
            console_write_char('\n');
            vm_input_buffer[vm_input_length] = '\0';
            return vm_store_heap_string(vm_input_buffer, vm_input_length, out_descriptor);
        }

        if (event.type == KEY_EVENT_BACKSPACE) {
            if (vm_input_length > 0U) {
                vm_input_length--;
                vm_input_buffer[vm_input_length] = '\0';
                vm_erase_last_echoed_char();
            }
            continue;
        }

        if (event.type != KEY_EVENT_CHARACTER) {
            continue;
        }

        if (event.character >= ' ' && event.character <= '~') {
            if (vm_input_length < BCVM_INPUT_MAX) {
                vm_input_buffer[vm_input_length++] = event.character;
                vm_input_buffer[vm_input_length] = '\0';
                console_write_char(event.character);
            }
        }
    }
}

static int vm_read_header(const uint8_t* image, uint32_t image_size, bcvm_image_header_t* out_header) {
    if (image == 0 || out_header == 0 || image_size < BCVM_IMAGE_HEADER_SIZE) {
        return -1;
    }

    out_header->magic = read_u32le(&image[0]);
    out_header->version = read_u16le(&image[4]);
    out_header->reserved = read_u16le(&image[6]);
    out_header->code_size = read_u32le(&image[8]);
    out_header->data_size = read_u32le(&image[12]);
    out_header->entry_offset = read_u32le(&image[16]);

    if (out_header->magic != BCVM_MAGIC
        || out_header->version < BCVM_MIN_COMPATIBLE_VERSION
        || out_header->version > BCVM_VERSION) {
        return -1;
    }

    if (out_header->code_size > (image_size - BCVM_IMAGE_HEADER_SIZE)) {
        return -1;
    }

    if (out_header->data_size > (image_size - BCVM_IMAGE_HEADER_SIZE - out_header->code_size)) {
        return -1;
    }

    if (out_header->entry_offset >= out_header->code_size) {
        return -1;
    }

    return 0;
}

static int bytecode_vm_run_context(const uint8_t* image, uint32_t image_size,
    const char* args, BytecodeVmContext* context, uint32_t entry_offset,
    int resume) {
    bcvm_image_header_t header;
    const uint8_t* code;
    const uint8_t* data;
    int32_t* stack;
    int32_t (*locals)[BCVM_LOCALS_MAX];
    uint32_t* call_return_pc;
    uint32_t sp;
    uint32_t call_depth;
    uint32_t pc;
    uint32_t steps;
    int fullscreen_enabled = 0;

    (void)args;
    active_vm_context = context;
    stack = vm_stack;
    locals = vm_locals;
    call_return_pc = vm_call_return_pc;

    if (vm_read_header(image, image_size, &header) != 0) {
        ERROR_LOG("BCVM invalid image header");
        return -1;
    }

    if (entry_offset == 0xFFFFFFFFU) {
        entry_offset = header.entry_offset;
    }
    if (entry_offset >= header.code_size) {
        return -1;
    }

    if (resume) {
        if (context->active_call == 0U
            || context->image != image
            || context->image_size != image_size
            || context->entry_offset != entry_offset) {
            return -1;
        }
        sp = context->sp;
        call_depth = context->call_depth;
        pc = context->pc;
        steps = context->steps;
    } else {
        vm_input_length = 0U;
        vm_input_buffer[0] = '\0';
        vm_heap_next_offset = 0U;
        vm_heap_next_slot = 0U;
        vm_heap_array_next_offset = 0U;
        vm_heap_array_next_slot = 0U;
        for (uint32_t i = 0U; i < BCVM_HEAP_STRING_MAX; ++i) {
            vm_heap_string_used[i] = 0U;
        }
        for (uint32_t i = 0U; i < BCVM_HEAP_ARRAY_MAX; ++i) {
            vm_heap_array_used[i] = 0U;
        }
        sp = 0U;
        call_depth = 0U;
        pc = entry_offset;
        steps = 0U;
        context->return_value = 0;
        context->stop_requested = 0U;

        {
            volatile int32_t* init_stack = &stack[0];
            volatile int32_t* init_locals = &locals[0][0];
            volatile uint32_t* init_returns = &call_return_pc[0];

            for (uint32_t i = 0U; i < BCVM_STACK_MAX; ++i) {
                init_stack[i] = 0;
            }
            for (uint32_t i = 0U;
                i < (BCVM_CALL_DEPTH_MAX + 1U) * BCVM_LOCALS_MAX; ++i) {
                init_locals[i] = 0;
            }
            for (uint32_t i = 0U; i < BCVM_CALL_DEPTH_MAX; ++i) {
                init_returns[i] = 0U;
            }
        }
        context->image = image;
        context->image_size = image_size;
        context->entry_offset = entry_offset;
        context->active_call = 1U;
    }

    {
        if (sp > BCVM_STACK_MAX || call_depth > BCVM_CALL_DEPTH_MAX) {
            context->active_call = 0U;
            return -1;
        }
    }

    code = image + BCVM_IMAGE_HEADER_SIZE;
    data = code + header.code_size;

    if (!resume) {
        DEBUG_LOG("BCVM started");
    }

    while (pc < header.code_size) {
        uint8_t opcode = code[pc++];
        steps++;

        if (!fullscreen_enabled && steps >= BCVM_MAX_STEPS) {
            break;
        }

        if (fullscreen_enabled && steps == 0xFFFFFFFFU) {
            steps = 0;
        }

        if (fullscreen_enabled && vm_poll_quit_request()) {
            console_set_cursor_visible(1);
            DEBUG_LOG("BCVM fullscreen quit shortcut triggered");
            return 0;
        }

        switch (opcode) {
            case BCVM_OP_PUSH_I32: {
                if (pc + 4U > header.code_size || sp >= BCVM_STACK_MAX) {
                    ERROR_LOG("BCVM PUSH_I32 fault");
                    return -1;
                }

                stack[sp++] = read_i32le(&code[pc]);
                pc += 4U;
                break;
            }
            case BCVM_OP_LOAD_LOCAL: {
                uint16_t index;
                if (pc + 2U > header.code_size || sp >= BCVM_STACK_MAX) {
                    ERROR_LOG("BCVM LOAD_LOCAL fault");
                    return -1;
                }

                index = read_u16le(&code[pc]);
                pc += 2U;
                if (index >= BCVM_LOCALS_MAX) {
                    return -1;
                }

                stack[sp++] = locals[call_depth][index];
                break;
            }
            case BCVM_OP_STORE_LOCAL: {
                uint16_t index;
                if (pc + 2U > header.code_size || sp == 0U) {
                    ERROR_LOG("BCVM STORE_LOCAL fault");
                    return -1;
                }

                index = read_u16le(&code[pc]);
                pc += 2U;
                if (index >= BCVM_LOCALS_MAX) {
                    return -1;
                }

                locals[call_depth][index] = stack[--sp];
                break;
            }
            case BCVM_OP_ADD:
            case BCVM_OP_SUB:
            case BCVM_OP_MUL:
            case BCVM_OP_DIV:
            case BCVM_OP_MOD: {
                int32_t rhs;
                int32_t lhs;
                if (sp < 2U) {
                    ERROR_LOG("BCVM arithmetic underflow");
                    return -1;
                }

                rhs = stack[--sp];
                lhs = stack[--sp];

                if (opcode == BCVM_OP_ADD) {
                    stack[sp++] = lhs + rhs;
                } else if (opcode == BCVM_OP_SUB) {
                    stack[sp++] = lhs - rhs;
                } else if (opcode == BCVM_OP_MUL) {
                    stack[sp++] = lhs * rhs;
                } else {
                    if (rhs == 0) {
                        ERROR_LOG("BCVM divide by zero");
                        return -1;
                    }
                    stack[sp++] = (opcode == BCVM_OP_DIV) ? (lhs / rhs) : (lhs % rhs);
                }
                break;
            }
            case BCVM_OP_BIT_AND:
            case BCVM_OP_BIT_OR:
            case BCVM_OP_BIT_XOR:
            case BCVM_OP_SHIFT_LEFT:
            case BCVM_OP_SHIFT_RIGHT: {
                uint32_t rhs;
                uint32_t lhs;
                if (sp < 2U) {
                    ERROR_LOG("BCVM bitwise operation underflow");
                    return -1;
                }

                rhs = (uint32_t)stack[--sp];
                lhs = (uint32_t)stack[--sp];
                if (opcode == BCVM_OP_BIT_AND) {
                    stack[sp++] = (int32_t)(lhs & rhs);
                } else if (opcode == BCVM_OP_BIT_OR) {
                    stack[sp++] = (int32_t)(lhs | rhs);
                } else if (opcode == BCVM_OP_BIT_XOR) {
                    stack[sp++] = (int32_t)(lhs ^ rhs);
                } else if (opcode == BCVM_OP_SHIFT_LEFT) {
                    stack[sp++] = (int32_t)(lhs << (rhs & 31U));
                } else {
                    stack[sp++] = (int32_t)(lhs >> (rhs & 31U));
                }
                break;
            }
            case BCVM_OP_FMUL:
            case BCVM_OP_FDIV: {
                int32_t rhs;
                int32_t lhs;
                int32_t result;

                if (sp < 2U) {
                    ERROR_LOG("BCVM float arithmetic underflow");
                    return -1;
                }

                rhs = stack[--sp];
                lhs = stack[--sp];

                if (opcode == BCVM_OP_FDIV && rhs == 0) {
                    ERROR_LOG("BCVM float divide by zero");
                    return -1;
                }

                if (opcode == BCVM_OP_FMUL) {
                    int32_t lhs_whole = lhs / BCVM_FLOAT_SCALE;
                    int32_t lhs_fraction = lhs % BCVM_FLOAT_SCALE;
                    result = lhs_whole * rhs + (lhs_fraction * rhs) / BCVM_FLOAT_SCALE;
                } else {
                    int32_t lhs_whole = lhs / rhs;
                    int32_t lhs_fraction = lhs % rhs;
                    result = lhs_whole * BCVM_FLOAT_SCALE + (lhs_fraction * BCVM_FLOAT_SCALE) / rhs;
                }

                stack[sp++] = (int32_t)result;
                break;
            }
            case BCVM_OP_NEG: {
                if (sp == 0U) {
                    return -1;
                }
                stack[sp - 1U] = -stack[sp - 1U];
                break;
            }
            case BCVM_OP_BIT_NOT:
                if (sp == 0U) {
                    return -1;
                }
                stack[sp - 1U] = (int32_t)~(uint32_t)stack[sp - 1U];
                break;
            case BCVM_OP_EQ:
            case BCVM_OP_NE:
            case BCVM_OP_LT:
            case BCVM_OP_LE:
            case BCVM_OP_GT:
            case BCVM_OP_GE: {
                int32_t rhs;
                int32_t lhs;
                int32_t result = 0;

                if (sp < 2U) {
                    ERROR_LOG("BCVM compare underflow");
                    return -1;
                }

                rhs = stack[--sp];
                lhs = stack[--sp];

                if (opcode == BCVM_OP_EQ) {
                    result = (lhs == rhs) ? 1 : 0;
                } else if (opcode == BCVM_OP_NE) {
                    result = (lhs != rhs) ? 1 : 0;
                } else if (opcode == BCVM_OP_LT) {
                    result = (lhs < rhs) ? 1 : 0;
                } else if (opcode == BCVM_OP_LE) {
                    result = (lhs <= rhs) ? 1 : 0;
                } else if (opcode == BCVM_OP_GT) {
                    result = (lhs > rhs) ? 1 : 0;
                } else {
                    result = (lhs >= rhs) ? 1 : 0;
                }

                stack[sp++] = result;
                break;
            }
            case BCVM_OP_JMP: {
                uint32_t target;
                if (pc + 4U > header.code_size) {
                    ERROR_LOG("BCVM JMP fault");
                    return -1;
                }

                target = read_u32le(&code[pc]);
                if (target >= header.code_size) {
                    return -1;
                }

                pc = target;
                break;
            }
            case BCVM_OP_JMP_IF_ZERO: {
                uint32_t target;
                int32_t cond;

                if (pc + 4U > header.code_size || sp == 0U) {
                    ERROR_LOG("BCVM JMP_IF_ZERO fault");
                    return -1;
                }

                target = read_u32le(&code[pc]);
                pc += 4U;
                cond = stack[--sp];

                if (cond == 0) {
                    if (target >= header.code_size) {
                        return -1;
                    }
                    pc = target;
                }
                break;
            }
            case BCVM_OP_CALL: {
                uint32_t target;
                volatile int32_t* frame_locals;

                if (pc + 4U > header.code_size || call_depth >= BCVM_CALL_DEPTH_MAX) {
                    ERROR_LOG("BCVM CALL fault");
                    return -1;
                }

                target = read_u32le(&code[pc]);
                pc += 4U;

                if (target >= header.code_size) {
                    return -1;
                }

                call_return_pc[call_depth] = pc;
                call_depth++;
                frame_locals = &locals[call_depth][0];
                for (uint32_t i = 0; i < BCVM_LOCALS_MAX; i++) {
                    frame_locals[i] = 0;
                }
                pc = target;
                break;
            }
            case BCVM_OP_CALL_ARGS: {
                uint32_t target;
                uint8_t arg_count;
                volatile int32_t* frame_locals;

                if (pc + 5U > header.code_size || call_depth >= BCVM_CALL_DEPTH_MAX) {
                    ERROR_LOG("BCVM CALL_ARGS fault");
                    return -1;
                }

                target = read_u32le(&code[pc]);
                pc += 4U;
                arg_count = code[pc++];

                if (target >= header.code_size || sp < arg_count) {
                    return -1;
                }

                call_return_pc[call_depth] = pc;
                call_depth++;

                frame_locals = &locals[call_depth][0];
                for (uint32_t i = 0; i < BCVM_LOCALS_MAX; i++) {
                    frame_locals[i] = 0;
                }

                for (uint32_t i = 0; i < (uint32_t)arg_count; i++) {
                    frame_locals[(uint32_t)arg_count - 1U - i] = stack[--sp];
                }

                pc = target;
                break;
            }
            case BCVM_OP_PRINT_STR: {
                uint16_t offset;
                uint16_t length;
                if (pc + 4U > header.code_size) {
                    ERROR_LOG("BCVM PRINT_STR fault");
                    return -1;
                }

                offset = read_u16le(&code[pc]);
                length = read_u16le(&code[pc + 2U]);
                pc += 4U;

                if ((uint32_t)offset + (uint32_t)length > header.data_size) {
                    return -1;
                }

                for (uint32_t i = 0; i < (uint32_t)length; i++) {
                    console_write_char((char)data[offset + i]);
                }
                break;
            }
            case BCVM_OP_SYSTEM: {
                const char* command;
                uint16_t command_length;
                int result = -1;

                if (sp == 0U
                    || vm_resolve_string_descriptor((uint32_t)stack[--sp],
                        data, &header, &command, &command_length) != 0) {
                    return -1;
                }

                if (vm_system_command_allowed(command, command_length)) {
                    char command_buffer[BCVM_INPUT_MAX + 1U];
                    if ((uint32_t)command_length + 1U <= sizeof(command_buffer)) {
                        for (uint16_t i = 0; i < command_length; i++) {
                            command_buffer[i] = command[i];
                        }
                        command_buffer[command_length] = '\0';
                        command_execute(command_buffer);
                        result = 0;
                    }
                }

                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                stack[sp++] = result;
                break;
            }
            case BCVM_OP_PRINT_INT: {
                if (sp == 0U) {
                    return -1;
                }

                vm_write_int(stack[--sp]);
                break;
            }
            case BCVM_OP_PRINT_FLOAT:
                if (sp == 0U) {
                    return -1;
                }

                vm_write_float(stack[--sp]);
                break;
            case BCVM_OP_PRINT_COLOR_STR: {
                uint16_t offset;
                uint16_t length;
                int32_t color_value;

                if (pc + 4U > header.code_size || sp == 0U) {
                    ERROR_LOG("BCVM PRINT_COLOR_STR fault");
                    return -1;
                }

                offset = read_u16le(&code[pc]);
                length = read_u16le(&code[pc + 2U]);
                pc += 4U;

                if ((uint32_t)offset + (uint32_t)length > header.data_size) {
                    return -1;
                }

                color_value = stack[--sp];
                if (color_value < 0) {
                    color_value = 0;
                }

                console_set_fg_color((VGA_Color)((uint32_t)color_value & 0x0FU));
                for (uint32_t i = 0; i < (uint32_t)length; i++) {
                    console_write_char((char)data[offset + i]);
                }
                console_set_fg_color(COLOR_WHITE);
                break;
            }
            case BCVM_OP_PRINT_STR_VAL: {
                const char* string_ptr;
                uint16_t string_len;

                if (sp == 0U) {
                    return -1;
                }

                if (vm_resolve_string_descriptor((uint32_t)stack[--sp], data, &header, &string_ptr, &string_len) != 0) {
                    return -1;
                }

                for (uint16_t i = 0; i < string_len; i++) {
                    console_write_char(string_ptr[i]);
                }
                break;
            }
            case BCVM_OP_PRINT_COLOR_STR_VAL: {
                const char* string_ptr;
                uint16_t string_len;
                int32_t color_value;

                if (sp < 2U) {
                    return -1;
                }

                color_value = stack[sp - 2U];
                if (vm_resolve_string_descriptor((uint32_t)stack[sp - 1U], data, &header, &string_ptr, &string_len) != 0) {
                    return -1;
                }

                sp -= 2U;
                console_set_fg_color((VGA_Color)((uint32_t)color_value & 0x0FU));
                for (uint16_t i = 0; i < string_len; i++) {
                    console_write_char(string_ptr[i]);
                }
                console_set_fg_color(COLOR_WHITE);
                break;
            }
            case BCVM_OP_PRINT_NL:
                console_write_char('\n');
                break;
            case BCVM_OP_PRINT_CHAR:
                if (sp == 0U) {
                    return -1;
                }

                console_write_char((char)(stack[--sp] & 0xFF));
                break;
            case BCVM_OP_READ_INT:
                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                stack[sp++] = vm_read_int();
                break;
            case BCVM_OP_READ_TEXT: {
                uint32_t descriptor;
                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                if (vm_read_text(&descriptor) != 0) {
                    return -1;
                }

                stack[sp++] = (int32_t)descriptor;
                break;
            }
            case BCVM_OP_PRINT_INPUT:
                for (uint16_t i = 0; i < vm_input_length; i++) {
                    console_write_char(vm_input_buffer[i]);
                }
                break;
            case BCVM_OP_INPUT_LEN:
                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                stack[sp++] = (int32_t)vm_input_length;
                break;
            case BCVM_OP_INPUT_EQ: {
                uint16_t offset;
                uint16_t length;
                int32_t equal = 1;

                if (pc + 4U > header.code_size || sp >= BCVM_STACK_MAX) {
                    ERROR_LOG("BCVM INPUT_EQ fault");
                    return -1;
                }

                offset = read_u16le(&code[pc]);
                length = read_u16le(&code[pc + 2U]);
                pc += 4U;

                if ((uint32_t)offset + (uint32_t)length > header.data_size) {
                    return -1;
                }

                if (length != vm_input_length) {
                    equal = 0;
                } else {
                    for (uint16_t i = 0; i < length; i++) {
                        if (vm_input_buffer[i] != (char)data[offset + i]) {
                            equal = 0;
                            break;
                        }
                    }
                }

                stack[sp++] = equal;
                break;
            }
            case BCVM_OP_STR_LEN: {
                const char* string_ptr;
                uint16_t string_len;

                if (sp == 0U) {
                    return -1;
                }

                if (vm_resolve_string_descriptor((uint32_t)stack[sp - 1U], data, &header, &string_ptr, &string_len) != 0) {
                    return -1;
                }

                (void)string_ptr;
                stack[sp - 1U] = (int32_t)string_len;
                break;
            }
            case BCVM_OP_STR_EQ: {
                const char* lhs_ptr;
                const char* rhs_ptr;
                uint16_t lhs_len;
                uint16_t rhs_len;
                int32_t equal = 1;

                if (sp < 2U) {
                    return -1;
                }

                if (vm_resolve_string_descriptor((uint32_t)stack[sp - 2U], data, &header, &lhs_ptr, &lhs_len) != 0) {
                    return -1;
                }

                if (vm_resolve_string_descriptor((uint32_t)stack[sp - 1U], data, &header, &rhs_ptr, &rhs_len) != 0) {
                    return -1;
                }

                if (lhs_len != rhs_len) {
                    equal = 0;
                } else {
                    for (uint16_t i = 0; i < lhs_len; i++) {
                        if (lhs_ptr[i] != rhs_ptr[i]) {
                            equal = 0;
                            break;
                        }
                    }
                }

                sp -= 2U;
                stack[sp++] = equal;
                break;
            }
            case BCVM_OP_ARR_NEW: {
                uint16_t length;
                uint32_t handle;

                if (pc + 2U > header.code_size || sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                length = read_u16le(&code[pc]);
                pc += 2U;
                if (vm_create_array(length, &handle) != 0) {
                    return -1;
                }

                stack[sp++] = (int32_t)handle;
                break;
            }
            case BCVM_OP_ARR_GET: {
                int32_t index;
                int32_t value;
                uint32_t handle;

                if (sp < 2U) {
                    return -1;
                }

                index = stack[--sp];
                handle = (uint32_t)stack[--sp];
                if (vm_get_array_value(handle, index, &value) != 0) {
                    return -1;
                }

                stack[sp++] = value;
                break;
            }
            case BCVM_OP_ARR_SET: {
                int32_t index;
                int32_t value;
                uint32_t handle;

                if (sp < 3U) {
                    return -1;
                }

                value = stack[--sp];
                index = stack[--sp];
                handle = (uint32_t)stack[--sp];
                if (vm_set_array_value(handle, index, value) != 0) {
                    return -1;
                }
                break;
            }
            case BCVM_OP_FILE_READ: {
                uint32_t path_desc;
                uint32_t size = 0;
                uint32_t text_desc;

                if (sp < 1U) {
                    return -1;
                }

                path_desc = (uint32_t)stack[--sp];
                if (vm_copy_string_descriptor_to_buffer(path_desc,
                        data,
                        &header,
                        vm_file_path_buffer,
                        sizeof(vm_file_path_buffer)) != 0) {
                    return -1;
                }

                if (vfs_read_file(vm_file_path_buffer,
                        vm_file_text_buffer,
                        BCVM_FILE_TEXT_MAX,
                        &size) != 0) {
                    return -1;
                }

                if (size > BCVM_FILE_TEXT_MAX) {
                    return -1;
                }

                vm_file_text_buffer[size] = '\0';
                if (vm_store_heap_string(vm_file_text_buffer, (uint16_t)size, &text_desc) != 0) {
                    return -1;
                }

                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                stack[sp++] = (int32_t)text_desc;
                break;
            }
            case BCVM_OP_FILE_WRITE: {
                uint32_t path_desc;
                uint32_t text_desc;
                const char* text_ptr;
                uint16_t text_len;
                int32_t status = -1;

                if (sp < 2U || sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                text_desc = (uint32_t)stack[--sp];
                path_desc = (uint32_t)stack[--sp];

                if (vm_copy_string_descriptor_to_buffer(path_desc,
                        data,
                        &header,
                        vm_file_path_buffer,
                        sizeof(vm_file_path_buffer)) == 0
                    && vm_resolve_string_descriptor(text_desc, data, &header, &text_ptr, &text_len) == 0) {
                    if (vfs_write_file(vm_file_path_buffer, text_ptr, text_len, 0) == 0) {
                        status = 0;
                    }
                }

                stack[sp++] = status;
                break;
            }
            case BCVM_OP_FILE_APPEND: {
                uint32_t path_desc;
                uint32_t text_desc;
                const char* text_ptr;
                uint16_t text_len;
                int32_t status = -1;

                if (sp < 2U || sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                text_desc = (uint32_t)stack[--sp];
                path_desc = (uint32_t)stack[--sp];

                if (vm_copy_string_descriptor_to_buffer(path_desc,
                        data,
                        &header,
                        vm_file_path_buffer,
                        sizeof(vm_file_path_buffer)) == 0
                    && vm_resolve_string_descriptor(text_desc, data, &header, &text_ptr, &text_len) == 0) {
                    if (vfs_write_file(vm_file_path_buffer, text_ptr, text_len, 1) == 0) {
                        status = 0;
                    }
                }

                stack[sp++] = status;
                break;
            }
            case BCVM_OP_FILE_EXISTS: {
                uint32_t path_desc;
                int is_dir = 0;
                int32_t exists = 0;

                if (sp < 1U || sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                path_desc = (uint32_t)stack[--sp];
                if (vm_copy_string_descriptor_to_buffer(path_desc,
                        data,
                        &header,
                        vm_file_path_buffer,
                        sizeof(vm_file_path_buffer)) == 0) {
                    if (vfs_path_is_dir(vm_file_path_buffer, &is_dir) == 0) {
                        exists = 1;
                    }
                }

                stack[sp++] = exists;
                break;
            }
            case BCVM_OP_SET_FULLSCREEN: {
                int32_t enabled;

                if (sp == 0U) {
                    return -1;
                }

                enabled = stack[--sp];
                fullscreen_enabled = enabled != 0;
                console_set_cursor_visible(fullscreen_enabled ? 0 : 1);
                break;
            }
            case BCVM_OP_APP_SHOULD_QUIT:
                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                stack[sp++] = vm_poll_quit_request() ? 1 : 0;
                break;
            case BCVM_OP_APP_READ_KEY: {
                KeyEvent event;
                if (sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                do {
                    event = keyboard_read_event();
                } while (event.type == KEY_EVENT_NONE);
                if (event.type == KEY_EVENT_CHARACTER) {
                    stack[sp++] = (unsigned char)event.character;
                } else if (event.type == KEY_EVENT_ENTER) {
                    stack[sp++] = '\n';
                } else if (event.type == KEY_EVENT_BACKSPACE) {
                    stack[sp++] = '\b';
                } else {
                    stack[sp++] = -1;
                }
                break;
            }
            case BCVM_OP_APP_WAIT_KEY_RELEASE:
                if (sp == 0U) {
                    return -1;
                }
                if (vm_wait_key_release((char)stack[sp - 1U]) == 0) {
                    stack[sp - 1U] = 0;
                } else {
                    stack[sp - 1U] = 1;
                }
                break;
            case BCVM_OP_DRIVER_SERVICE_CALL: {
                int32_t argument1;
                int32_t argument0;
                int32_t operation;
                int32_t driver_id;
                if (sp < 4U) {
                    return -1;
                }
                argument1 = stack[--sp];
                argument0 = stack[--sp];
                operation = stack[--sp];
                driver_id = stack[--sp];
                stack[sp++] = app_manager_driver_service_call(
                    (uint32_t)driver_id, operation, argument0, argument1);
                break;
            }
            case BCVM_OP_DRIVER_SERVICE_FIND: {
                int32_t service_type;
                if (sp == 0U) {
                    return -1;
                }
                service_type = stack[--sp];
                stack[sp++] = app_manager_driver_service_find(service_type);
                break;
            }
            case BCVM_OP_APP_EXIT:
                console_set_cursor_visible(1);
                DEBUG_LOG("BCVM app exit requested");
                return 0;
            case BCVM_OP_DRAW_PIXEL: {
                int32_t x;
                int32_t y;
                int32_t color;

                if (sp < 3U) {
                    return -1;
                }

                color = stack[--sp];
                y = stack[--sp];
                x = stack[--sp];

                if (color < 0) {
                    color = 0;
                }

                if (color > 15) {
                    color = 15;
                }

                console_draw_pixel((int)x, (int)y, (VGA_Color)color);
                break;
            }
            case BCVM_OP_DRIVER_SERIAL_WRITE:
            case BCVM_OP_DRIVER_SERIAL_READ:
            case BCVM_OP_DRIVER_PORT_READ8:
            case BCVM_OP_DRIVER_PORT_WRITE8:
            case BCVM_OP_DRIVER_MMIO_READ32:
            case BCVM_OP_DRIVER_MMIO_WRITE32:
            case BCVM_OP_DRIVER_SHOULD_STOP:
            case BCVM_OP_DRIVER_PCI_FIND:
            case BCVM_OP_DRIVER_PCI_READ_CONFIG32:
            case BCVM_OP_DRIVER_PCI_READ_BAR32:
            case BCVM_OP_DRIVER_PCI_CLAIM:
            case BCVM_OP_DRIVER_PCI_ENABLE:
            case BCVM_OP_DRIVER_PCI_CLAIM_IO_BAR:
            case BCVM_OP_DRIVER_PCI_IO_READ:
            case BCVM_OP_DRIVER_PCI_IO_WRITE:
            case BCVM_OP_DRIVER_DMA_ALLOC:
            case BCVM_OP_DRIVER_DMA_PHYSICAL:
            case BCVM_OP_DRIVER_DMA_READ:
            case BCVM_OP_DRIVER_DMA_WRITE:
            case BCVM_OP_DRIVER_DMA_FREE:
            case BCVM_OP_DRIVER_PCI_DISABLE:
            case BCVM_OP_DRIVER_PCI_CLAIM_MMIO_BAR:
            case BCVM_OP_DRIVER_PCI_MMIO_READ32:
            case BCVM_OP_DRIVER_PCI_MMIO_WRITE32:
            case BCVM_OP_DRIVER_DMA_ALLOC_BELOW:
            case BCVM_OP_DRIVER_STATE_READ:
            case BCVM_OP_DRIVER_STATE_WRITE:
            case BCVM_OP_DRIVER_PCI_SUBSCRIBE_IRQ:
            case BCVM_OP_DRIVER_IRQ_READ:
            case BCVM_OP_DRIVER_IRQ_DROPPED:
            case BCVM_OP_DRIVER_IRQ_ACK:
                ERROR_LOG("BCVM driver opcode rejected outside ring-3 VM");
                return -1;
            case BCVM_OP_POP:
                if (sp == 0U) {
                    return -1;
                }
                sp--;
                break;
            case BCVM_OP_RET:
                if (sp == 0U) {
                    ERROR_LOG("BCVM RET fault");
                    return -1;
                }

                if (call_depth == 0U) {
                    context->return_value = stack[sp - 1U];
                    console_set_cursor_visible(1);
                    DEBUG_LOG("BCVM finished");
                    return 0;
                }

                {
                    int32_t ret_value = stack[--sp];
                    call_depth--;
                    if (sp >= BCVM_STACK_MAX) {
                        return -1;
                    }
                    stack[sp++] = ret_value;
                    pc = call_return_pc[call_depth];
                }
                break;
            case BCVM_OP_HALT:
                context->return_value = 0;
                console_set_cursor_visible(1);
                DEBUG_LOG("BCVM finished");
                return 0;
            default:
                ERROR_LOG("BCVM unknown opcode");
                return -1;
        }
    }

    ERROR_LOG("BCVM execution aborted");
    return -1;
}

int bytecode_vm_run(const uint8_t* image, uint32_t image_size, const char* args)
{
    return bytecode_vm_run_context(image, image_size, args,
        &app_vm_context, 0xFFFFFFFFU, 0);
}

int bytecode_vm_driver_slice(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int resume)
{
    return user_driver_vm_slice(image, image_size, entry_offset,
        runtime_handle, resume);
}

int bytecode_vm_driver_service(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int32_t operation,
    int32_t argument0, int32_t argument1, int resume)
{
    return user_driver_vm_service(image, image_size, entry_offset,
        runtime_handle, operation, argument0, argument1, resume);
}

int bytecode_vm_driver_service_find(int32_t service_type)
{
    return app_manager_driver_service_find(service_type);
}

void bytecode_vm_driver_reset(uint32_t runtime_handle)
{
    user_driver_vm_reset(runtime_handle);
}

void bytecode_vm_driver_destroy(uint32_t runtime_handle)
{
    user_driver_vm_destroy(runtime_handle);
}

int32_t bytecode_vm_driver_result(uint32_t runtime_handle)
{
    return user_driver_vm_result(runtime_handle);
}

int bytecode_vm_driver_stop_requested(uint32_t runtime_handle)
{
    return user_driver_vm_stop_requested(runtime_handle);
}

int bytecode_vm_driver_isolation_available(void)
{
    return user_driver_vm_isolation_available();
}

int bytecode_vm_run_driver(const uint8_t* image, uint32_t image_size, const char* args) {
    int result;
    int resume = 0;
    int runtime_handle;
    (void)args;

    if (!bytecode_vm_driver_isolation_available()) {
        ERROR_LOG("PrismCC driver execution rejected: ring-3 VM self-test unavailable");
        return -1;
    }
    runtime_handle = driver_runtime_create();
    if (runtime_handle < 0) {
        ERROR_LOG("PrismCC driver runtime could not claim hardware services");
        return -1;
    }

    do {
        result = bytecode_vm_driver_slice(image, image_size,
            0xFFFFFFFFU, (uint32_t)runtime_handle, resume);
        resume = result == BCVM_DRIVER_SLICE_YIELDED;
    } while (resume);
    bytecode_vm_driver_destroy((uint32_t)runtime_handle);
    driver_runtime_destroy((uint32_t)runtime_handle);
    return result;
}
