#include "apps/bytecode_vm.h"

#include "apps/app_format.h"
#include "debug/log.h"
#include "display/console.h"
#include "drivers/driver_api.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"

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

static char vm_input_buffer[BCVM_INPUT_MAX + 1U];
static uint16_t vm_input_length = 0;
static char vm_heap_string_data[BCVM_HEAP_STRING_BYTES];
static uint16_t vm_heap_string_offset[BCVM_HEAP_STRING_MAX];
static uint16_t vm_heap_string_length[BCVM_HEAP_STRING_MAX];
static uint8_t vm_heap_string_used[BCVM_HEAP_STRING_MAX];
static uint16_t vm_heap_next_offset = 0;
static uint8_t vm_heap_next_slot = 0;
static int32_t vm_heap_array_data[BCVM_HEAP_ARRAY_CELLS];
static uint16_t vm_heap_array_offset[BCVM_HEAP_ARRAY_MAX];
static uint16_t vm_heap_array_length[BCVM_HEAP_ARRAY_MAX];
static uint8_t vm_heap_array_used[BCVM_HEAP_ARRAY_MAX];
static uint16_t vm_heap_array_next_offset = 0;
static uint8_t vm_heap_array_next_slot = 0;
static int32_t vm_stack[BCVM_STACK_MAX];
static int32_t vm_locals[BCVM_CALL_DEPTH_MAX + 1U][BCVM_LOCALS_MAX];
static uint32_t vm_call_return_pc[BCVM_CALL_DEPTH_MAX];
static char vm_file_path_buffer[BCVM_FILE_PATH_MAX];
static char vm_file_text_buffer[BCVM_FILE_TEXT_MAX + 1U];
static int vm_driver_mode;

#define VM_STRING_DESC_HEAP_MASK 0x80000000U

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

    if (out_header->magic != BCVM_MAGIC || out_header->version != BCVM_VERSION) {
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

int bytecode_vm_run(const uint8_t* image, uint32_t image_size, const char* args) {
    bcvm_image_header_t header;
    const uint8_t* code;
    const uint8_t* data;
    int32_t* stack = vm_stack;
    int32_t (*locals)[BCVM_LOCALS_MAX] = vm_locals;
    uint32_t* call_return_pc = vm_call_return_pc;
    uint32_t sp = 0;
    uint32_t call_depth = 0;
    uint32_t pc = 0;
    uint32_t steps = 0;
    int fullscreen_enabled = 0;

    (void)args;
    vm_input_length = 0;
    vm_input_buffer[0] = '\0';
    vm_heap_next_offset = 0;
    vm_heap_next_slot = 0;
    vm_heap_array_next_offset = 0;
    vm_heap_array_next_slot = 0;
    for (uint32_t i = 0; i < BCVM_HEAP_STRING_MAX; i++) {
        vm_heap_string_used[i] = 0U;
    }
    for (uint32_t i = 0; i < BCVM_HEAP_ARRAY_MAX; i++) {
        vm_heap_array_used[i] = 0U;
    }

    if (vm_read_header(image, image_size, &header) != 0) {
        ERROR_LOG("BCVM invalid image header");
        return -1;
    }

    {
        volatile int32_t* init_stack = &stack[0];
        volatile int32_t* init_locals = &locals[0][0];
        volatile uint32_t* init_returns = &call_return_pc[0];

        for (uint32_t i = 0; i < BCVM_STACK_MAX; i++) {
            init_stack[i] = 0;
        }

        for (uint32_t i = 0; i < (BCVM_CALL_DEPTH_MAX + 1U) * BCVM_LOCALS_MAX; i++) {
            init_locals[i] = 0;
        }

        for (uint32_t i = 0; i < BCVM_CALL_DEPTH_MAX; i++) {
            init_returns[i] = 0U;
        }
    }

    code = image + BCVM_IMAGE_HEADER_SIZE;
    data = code + header.code_size;
    pc = header.entry_offset;

    DEBUG_LOG("BCVM started");

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
            case BCVM_OP_DRIVER_SERIAL_WRITE: {
                const char* bytes;
                uint16_t length;
                int32_t status;

                if (!vm_driver_mode || sp == 0U || sp >= BCVM_STACK_MAX) {
                    return -1;
                }

                if (vm_resolve_string_descriptor((uint32_t)stack[--sp], data, &header, &bytes, &length) != 0) {
                    return -1;
                }
                status = driver_api_serial_write(bytes, length);
                stack[sp++] = status;
                break;
            }
            case BCVM_OP_DRIVER_SERIAL_READ:
                if (!vm_driver_mode || sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                stack[sp++] = driver_api_serial_read();
                break;
            case BCVM_OP_DRIVER_PORT_READ8:
                if (!vm_driver_mode || sp == 0U) {
                    return -1;
                }
                stack[sp - 1U] = driver_api_port_read8((uint32_t)stack[sp - 1U]);
                break;
            case BCVM_OP_DRIVER_PORT_WRITE8: {
                uint32_t port;
                uint32_t value;

                if (!vm_driver_mode || sp < 2U) {
                    return -1;
                }
                value = (uint32_t)stack[--sp];
                port = (uint32_t)stack[--sp];
                stack[sp++] = driver_api_port_write8(port, value);
                break;
            }
            case BCVM_OP_DRIVER_MMIO_READ32:
                if (!vm_driver_mode || sp == 0U) {
                    return -1;
                }
                stack[sp - 1U] = driver_api_mmio_read32((uint32_t)stack[sp - 1U]);
                break;
            case BCVM_OP_DRIVER_MMIO_WRITE32: {
                uint32_t address;
                uint32_t value;

                if (!vm_driver_mode || sp < 2U) {
                    return -1;
                }
                value = (uint32_t)stack[--sp];
                address = (uint32_t)stack[--sp];
                stack[sp++] = driver_api_mmio_write32(address, value);
                break;
            }
            case BCVM_OP_DRIVER_SHOULD_STOP:
                if (!vm_driver_mode || sp >= BCVM_STACK_MAX) {
                    return -1;
                }
                /* Driver loops must explicitly poll for shutdown; each poll renews the VM budget. */
                steps = 0U;
                stack[sp++] = vm_poll_quit_request() ? 1 : 0;
                break;
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

int bytecode_vm_run_driver(const uint8_t* image, uint32_t image_size, const char* args) {
    int result;

    if (driver_runtime_begin() != 0) {
        ERROR_LOG("PrismCC driver runtime could not claim hardware services");
        return -1;
    }

    vm_driver_mode = 1;
    result = bytecode_vm_run(image, image_size, args);
    vm_driver_mode = 0;
    driver_runtime_end();
    return result;
}
