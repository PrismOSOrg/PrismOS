#include "apps/user_driver_vm.h"

#include "apps/app_format.h"
#include "apps/bytecode_vm.h"
#include "debug/log.h"
#include "drivers/driver_api.h"
#include "memory/paging.h"
#include "memory/pmm.h"
#include "platform/user_mode.h"

#define USER_VM_CODE __attribute__((section(".user_vm.text"), noinline))
#define USER_VM_STACK_MAX 256U
#define USER_VM_LOCALS_MAX 512U
#define USER_VM_CALL_DEPTH_MAX 32U
#define USER_VM_HEAP_STRING_MAX 64U
#define USER_VM_HEAP_STRING_BYTES 4096U
#define USER_VM_HEAP_ARRAY_MAX 64U
#define USER_VM_HEAP_ARRAY_CELLS 1024U
#define USER_VM_SLICE_STEPS 5000U
#define USER_VM_PREEMPT_CHECK_INTERVAL 128U
#define USER_VM_KERNEL_STACK_PAGES 2U
#define USER_VM_USER_STACK_PAGES 4U
#define USER_VM_PHYSICAL_LIMIT 0x04000000ULL

typedef struct {
    uint32_t image_address;
    uint32_t image_size;
    uint32_t entry_offset;
    uint32_t state_address;
    uint32_t resume;
    uint32_t step_budget;
    uint32_t argument_count;
    int32_t arguments[3];
} user_vm_launch_t;

typedef struct {
    char heap_string_data[USER_VM_HEAP_STRING_BYTES];
    uint16_t heap_string_offset[USER_VM_HEAP_STRING_MAX];
    uint16_t heap_string_length[USER_VM_HEAP_STRING_MAX];
    uint8_t heap_string_used[USER_VM_HEAP_STRING_MAX];
    uint16_t heap_next_offset;
    uint8_t heap_next_slot;
    int32_t heap_array_data[USER_VM_HEAP_ARRAY_CELLS];
    uint16_t heap_array_offset[USER_VM_HEAP_ARRAY_MAX];
    uint16_t heap_array_length[USER_VM_HEAP_ARRAY_MAX];
    uint8_t heap_array_used[USER_VM_HEAP_ARRAY_MAX];
    uint16_t heap_array_next_offset;
    uint8_t heap_array_next_slot;
    int32_t stack[USER_VM_STACK_MAX];
    int32_t locals[USER_VM_CALL_DEPTH_MAX + 1U][USER_VM_LOCALS_MAX];
    uint32_t call_return_pc[USER_VM_CALL_DEPTH_MAX];
    uint32_t image_address;
    uint32_t image_size;
    uint32_t entry_offset;
    uint32_t pc;
    uint32_t sp;
    uint32_t call_depth;
    uint32_t steps;
    int32_t return_value;
    uint8_t active_call;
} user_vm_state_t;

typedef struct {
    paging_user_space_t address_space;
    user_mode_context_t mode;
    void* image_pages;
    void* state_pages;
    void* user_stack_pages;
    void* kernel_stack_pages;
    uint32_t image_page_count;
    uint32_t state_page_count;
    uint32_t user_stack_page_count;
    uint32_t image_user_address;
    uint32_t state_user_address;
    uint32_t vm_state_user_address;
    uint32_t service_vm_state_user_address;
    uint32_t user_stack_top;
    uint32_t kernel_stack_top;
    uint32_t section_page_count;
    const uint8_t* source_image;
    uint32_t source_image_size;
    int32_t return_value;
    uint8_t allocated;
} user_driver_runtime_t;

static user_driver_runtime_t user_driver_runtimes[DRIVER_RUNTIME_MAX];
static int driver_vm_self_test_passed;

extern uint8_t __user_vm_start;
extern uint8_t __user_vm_end;
extern uint8_t __user_vm_load_start;
extern void user_driver_vm_entry(const user_vm_launch_t* launch);

#define USER_VM_STRING_HEAP_MASK 0x80000000U

static USER_VM_CODE uint8_t uvm_read_u8(const uint8_t* image, uint32_t image_size,
    uint32_t offset, int* valid)
{
    if (offset >= image_size) {
        *valid = 0;
        return 0U;
    }
    return image[offset];
}

static USER_VM_CODE uint16_t uvm_read_u16(const uint8_t* image,
    uint32_t image_size, uint32_t offset, int* valid)
{
    if (offset > image_size || image_size - offset < 2U) {
        *valid = 0;
        return 0U;
    }
    return (uint16_t)(image[offset] | ((uint16_t)image[offset + 1U] << 8));
}

static USER_VM_CODE uint32_t uvm_read_u32(const uint8_t* image,
    uint32_t image_size, uint32_t offset, int* valid)
{
    if (offset > image_size || image_size - offset < 4U) {
        *valid = 0;
        return 0U;
    }
    return (uint32_t)image[offset]
        | ((uint32_t)image[offset + 1U] << 8)
        | ((uint32_t)image[offset + 2U] << 16)
        | ((uint32_t)image[offset + 3U] << 24);
}

static USER_VM_CODE int32_t uvm_syscall(uint32_t number, uint32_t first,
    uint32_t second, uint32_t third, uint32_t fourth)
{
    uint32_t result;
    __asm__ volatile ("int $0x80"
        : "=a"(result)
        : "0"(number), "b"(first), "c"(second), "d"(third), "S"(fourth)
        : "memory", "cc");
    return (int32_t)result;
}

static USER_VM_CODE int uvm_resolve_string(user_vm_state_t* state,
    const uint8_t* data, uint32_t data_size, uint32_t descriptor,
    const char** out_text, uint16_t* out_length)
{
    if ((descriptor & USER_VM_STRING_HEAP_MASK) != 0U) {
        uint8_t slot = (uint8_t)(descriptor & 0xFFU);
        if (slot >= USER_VM_HEAP_STRING_MAX || state->heap_string_used[slot] == 0U) {
            return -1;
        }
        *out_text = &state->heap_string_data[state->heap_string_offset[slot]];
        *out_length = state->heap_string_length[slot];
        return 0;
    }

    {
        uint16_t offset = (uint16_t)((descriptor >> 16) & 0x7FFFU);
        uint16_t length = (uint16_t)(descriptor & 0xFFFFU);
        if ((uint32_t)offset > data_size
            || (uint32_t)length > data_size - (uint32_t)offset) {
            return -1;
        }
        *out_text = (const char*)&data[offset];
        *out_length = length;
    }
    return 0;
}

static USER_VM_CODE int uvm_push(user_vm_state_t* state, int32_t value)
{
    if (state->sp >= USER_VM_STACK_MAX) {
        return -1;
    }
    state->stack[state->sp++] = value;
    return 0;
}

static USER_VM_CODE uint32_t uvm_integer_text(int32_t value, char* output)
{
    uint32_t magnitude = (uint32_t)value;
    uint32_t length = 0U;
    if (value < 0) magnitude = 0U - magnitude;
    do {
        output[length++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U);
    if (value < 0) output[length++] = '-';
    for (uint32_t index = 0U; index < length / 2U; ++index) {
        char temp = output[index];
        output[index] = output[length - index - 1U];
        output[length - index - 1U] = temp;
    }
    return length;
}

static USER_VM_CODE int uvm_print_fixed(int32_t value)
{
    char text[16];
    uint32_t length = 0U;
    int32_t whole = value / 1000;
    uint32_t fraction = (uint32_t)(value % 1000);
    if (value < 0 && fraction != 0U) {
        fraction = 0U - fraction;
    }
    length = uvm_integer_text(whole, text);
    text[length++] = '.';
    text[length++] = (char)('0' + fraction / 100U);
    text[length++] = (char)('0' + (fraction / 10U) % 10U);
    text[length++] = (char)('0' + fraction % 10U);
    return uvm_syscall(USER_SYSCALL_DRIVER_CONSOLE_WRITE,
        (uint32_t)(uintptr_t)text, length, 0U, 0U) < 0 ? -1 : 0;
}

static USER_VM_CODE int uvm_new_array(user_vm_state_t* state, uint16_t length,
    uint32_t* out_handle)
{
    uint8_t slot;

    if (length == 0U || length > USER_VM_HEAP_ARRAY_CELLS) {
        return -1;
    }
    if ((uint32_t)state->heap_array_next_offset + length > USER_VM_HEAP_ARRAY_CELLS) {
        state->heap_array_next_offset = 0U;
        state->heap_array_next_slot = 0U;
        for (uint32_t index = 0U; index < USER_VM_HEAP_ARRAY_MAX; ++index) {
            state->heap_array_used[index] = 0U;
        }
    }
    if (state->heap_array_next_slot >= USER_VM_HEAP_ARRAY_MAX) {
        state->heap_array_next_slot = 0U;
    }
    slot = state->heap_array_next_slot++;
    state->heap_array_offset[slot] = state->heap_array_next_offset;
    state->heap_array_length[slot] = length;
    state->heap_array_used[slot] = 1U;
    for (uint16_t index = 0U; index < length; ++index) {
        state->heap_array_data[state->heap_array_next_offset + index] = 0;
    }
    state->heap_array_next_offset = (uint16_t)(state->heap_array_next_offset + length);
    *out_handle = (uint32_t)slot + 1U;
    return 0;
}

static USER_VM_CODE int uvm_run(const user_vm_launch_t* launch)
{
    user_vm_state_t* state = (user_vm_state_t*)(uintptr_t)launch->state_address;
    const uint8_t* image = (const uint8_t*)(uintptr_t)launch->image_address;
    uint32_t image_size = launch->image_size;
    uint32_t entry_offset = launch->entry_offset;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t code_address;
    uint32_t data_address;
    uint32_t slice_steps = 0U;
    int valid = 1;

    if (state == 0 || image == 0 || image_size < BCVM_IMAGE_HEADER_SIZE
        || launch->image_address < PAGING_USER_BASE
        || launch->image_address >= PAGING_USER_BASE + PAGING_USER_SIZE
        || image_size > PAGING_USER_BASE + PAGING_USER_SIZE - launch->image_address) {
        return -1;
    }

    if (uvm_read_u32(image, image_size, 0U, &valid) != BCVM_MAGIC
        || uvm_read_u16(image, image_size, 4U, &valid) < BCVM_MIN_COMPATIBLE_VERSION
        || uvm_read_u16(image, image_size, 4U, &valid) > BCVM_VERSION) {
        return -1;
    }
    code_size = uvm_read_u32(image, image_size, 8U, &valid);
    data_size = uvm_read_u32(image, image_size, 12U, &valid);
    if (entry_offset == 0xFFFFFFFFU) {
        entry_offset = uvm_read_u32(image, image_size, 16U, &valid);
    }
    if (!valid || code_size > image_size - BCVM_IMAGE_HEADER_SIZE
        || data_size > image_size - BCVM_IMAGE_HEADER_SIZE - code_size
        || entry_offset >= code_size
        || launch->step_budget == 0U) {
        return -1;
    }
    code_address = launch->image_address + BCVM_IMAGE_HEADER_SIZE;
    data_address = code_address + code_size;

    if (launch->resume == 0U) {
        uint8_t* state_bytes = (uint8_t*)state;
        for (uint32_t index = 0U; index < sizeof(*state); ++index) {
            state_bytes[index] = 0U;
        }
        state->image_address = launch->image_address;
        state->image_size = image_size;
        state->entry_offset = entry_offset;
        state->pc = entry_offset;
        state->active_call = 1U;
        if (launch->argument_count > 3U) {
            return -1;
        }
        for (uint32_t index = 0U; index < launch->argument_count; ++index) {
            state->locals[0][index] = launch->arguments[index];
        }
    } else if (state->active_call == 0U
        || state->image_address != launch->image_address
        || state->image_size != image_size
        || state->entry_offset != entry_offset
        || state->sp > USER_VM_STACK_MAX
        || state->call_depth > USER_VM_CALL_DEPTH_MAX) {
        return -1;
    }

    while (state->pc < code_size) {
        uint8_t opcode;
        uint32_t operand;
        int32_t left;
        int32_t right;

        if (slice_steps >= launch->step_budget) {
            return BCVM_DRIVER_SLICE_YIELDED;
        }
        opcode = uvm_read_u8((const uint8_t*)(uintptr_t)code_address,
            code_size, state->pc++, &valid);
        if (!valid) {
            break;
        }
        ++state->steps;
        ++slice_steps;

        switch (opcode) {
            case BCVM_OP_PUSH_I32:
                operand = uvm_read_u32((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || uvm_push(state, (int32_t)operand) != 0) {
                    goto failed;
                }
                state->pc += 4U;
                break;
            case BCVM_OP_LOAD_LOCAL: {
                uint16_t index = uvm_read_u16((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || index >= USER_VM_LOCALS_MAX
                    || uvm_push(state, state->locals[state->call_depth][index]) != 0) {
                    goto failed;
                }
                state->pc += 2U;
                break;
            }
            case BCVM_OP_STORE_LOCAL: {
                uint16_t index = uvm_read_u16((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || state->sp == 0U || index >= USER_VM_LOCALS_MAX) {
                    goto failed;
                }
                state->pc += 2U;
                state->locals[state->call_depth][index] = state->stack[--state->sp];
                break;
            }
            case BCVM_OP_ADD:
            case BCVM_OP_SUB:
            case BCVM_OP_MUL:
            case BCVM_OP_DIV:
            case BCVM_OP_MOD:
            case BCVM_OP_BIT_AND:
            case BCVM_OP_BIT_OR:
            case BCVM_OP_BIT_XOR:
            case BCVM_OP_SHIFT_LEFT:
            case BCVM_OP_SHIFT_RIGHT:
            case BCVM_OP_FMUL:
            case BCVM_OP_FDIV:
            case BCVM_OP_EQ:
            case BCVM_OP_NE:
            case BCVM_OP_LT:
            case BCVM_OP_LE:
            case BCVM_OP_GT:
            case BCVM_OP_GE: {
                uint32_t unsigned_left;
                uint32_t unsigned_right;
                int32_t result;
                if (state->sp < 2U) {
                    goto failed;
                }
                right = state->stack[--state->sp];
                left = state->stack[--state->sp];
                unsigned_left = (uint32_t)left;
                unsigned_right = (uint32_t)right;
                if (opcode == BCVM_OP_ADD) result = left + right;
                else if (opcode == BCVM_OP_SUB) result = left - right;
                else if (opcode == BCVM_OP_MUL) result = left * right;
                else if (opcode == BCVM_OP_DIV || opcode == BCVM_OP_MOD) {
                    if (right == 0) goto failed;
                    result = opcode == BCVM_OP_DIV ? left / right : left % right;
                } else if (opcode == BCVM_OP_BIT_AND) result = (int32_t)(unsigned_left & unsigned_right);
                else if (opcode == BCVM_OP_BIT_OR) result = (int32_t)(unsigned_left | unsigned_right);
                else if (opcode == BCVM_OP_BIT_XOR) result = (int32_t)(unsigned_left ^ unsigned_right);
                else if (opcode == BCVM_OP_SHIFT_LEFT) result = (int32_t)(unsigned_left << (unsigned_right & 31U));
                else if (opcode == BCVM_OP_SHIFT_RIGHT) result = (int32_t)(unsigned_left >> (unsigned_right & 31U));
                else if (opcode == BCVM_OP_FMUL) {
                    int32_t whole = left / 1000;
                    int32_t fraction = left % 1000;
                    result = whole * right + (fraction * right) / 1000;
                } else if (opcode == BCVM_OP_FDIV) {
                    if (right == 0) goto failed;
                    result = (left / right) * 1000 + ((left % right) * 1000) / right;
                } else if (opcode == BCVM_OP_EQ) result = left == right;
                else if (opcode == BCVM_OP_NE) result = left != right;
                else if (opcode == BCVM_OP_LT) result = left < right;
                else if (opcode == BCVM_OP_LE) result = left <= right;
                else if (opcode == BCVM_OP_GT) result = left > right;
                else result = left >= right;
                if (uvm_push(state, result) != 0) goto failed;
                break;
            }
            case BCVM_OP_NEG:
            case BCVM_OP_BIT_NOT:
                if (state->sp == 0U) goto failed;
                if (opcode == BCVM_OP_NEG) state->stack[state->sp - 1U] = -state->stack[state->sp - 1U];
                else state->stack[state->sp - 1U] = (int32_t)~(uint32_t)state->stack[state->sp - 1U];
                break;
            case BCVM_OP_JMP: {
                uint32_t target = uvm_read_u32((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || target >= code_size) goto failed;
                state->pc = target;
                break;
            }
            case BCVM_OP_JMP_IF_ZERO: {
                uint32_t target = uvm_read_u32((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || state->pc > code_size || code_size - state->pc < 4U
                    || state->sp == 0U) goto failed;
                state->pc += 4U;
                if (state->stack[--state->sp] == 0) {
                    if (target >= code_size) goto failed;
                    state->pc = target;
                }
                break;
            }
            case BCVM_OP_CALL:
            case BCVM_OP_CALL_ARGS: {
                uint32_t target = uvm_read_u32((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                uint32_t arg_count = 0U;
                if (!valid || target >= code_size || state->call_depth >= USER_VM_CALL_DEPTH_MAX) {
                    goto failed;
                }
                state->pc += 4U;
                if (opcode == BCVM_OP_CALL_ARGS) {
                    arg_count = uvm_read_u8((const uint8_t*)(uintptr_t)code_address,
                        code_size, state->pc++, &valid);
                    if (!valid || state->sp < arg_count) goto failed;
                }
                state->call_return_pc[state->call_depth] = state->pc;
                ++state->call_depth;
                for (uint32_t index = 0U; index < USER_VM_LOCALS_MAX; ++index) {
                    state->locals[state->call_depth][index] = 0;
                }
                for (uint32_t index = 0U; index < arg_count; ++index) {
                    state->locals[state->call_depth][arg_count - 1U - index]
                        = state->stack[--state->sp];
                }
                state->pc = target;
                break;
            }
            case BCVM_OP_PRINT_STR:
            case BCVM_OP_PRINT_COLOR_STR: {
                uint16_t offset = uvm_read_u16((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                uint16_t length = uvm_read_u16((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc + 2U, &valid);
                if (!valid || (uint32_t)offset > data_size
                    || (uint32_t)length > data_size - offset
                    || (opcode == BCVM_OP_PRINT_COLOR_STR && state->sp == 0U)) {
                    goto failed;
                }
                if (opcode == BCVM_OP_PRINT_COLOR_STR) {
                    --state->sp;
                }
                if (uvm_syscall(USER_SYSCALL_DRIVER_CONSOLE_WRITE,
                        data_address + offset, length, 0U, 0U) < 0) {
                    goto failed;
                }
                state->pc += 4U;
                break;
            }
            case BCVM_OP_PRINT_STR_VAL:
            case BCVM_OP_PRINT_COLOR_STR_VAL:
            case BCVM_OP_DRIVER_SERIAL_WRITE: {
                const char* text;
                uint16_t length;
                uint32_t descriptor;
                int32_t result;
                if (state->sp < (opcode == BCVM_OP_PRINT_COLOR_STR_VAL ? 2U : 1U)) {
                    goto failed;
                }
                if (opcode == BCVM_OP_PRINT_COLOR_STR_VAL) {
                    descriptor = (uint32_t)state->stack[state->sp - 1U];
                    state->sp -= 2U;
                } else {
                    descriptor = (uint32_t)state->stack[--state->sp];
                }
                if (uvm_resolve_string(state, (const uint8_t*)(uintptr_t)data_address,
                        data_size, descriptor, &text, &length) != 0) {
                    goto failed;
                }
                result = uvm_syscall(opcode == BCVM_OP_DRIVER_SERIAL_WRITE
                        ? USER_SYSCALL_DRIVER_SERIAL_WRITE
                        : USER_SYSCALL_DRIVER_CONSOLE_WRITE,
                    (uint32_t)(uintptr_t)text, length, 0U, 0U);
                if (result < 0) {
                    goto failed;
                }
                if (opcode == BCVM_OP_DRIVER_SERIAL_WRITE
                    && uvm_push(state, result) != 0) {
                    goto failed;
                }
                break;
            }
            case BCVM_OP_PRINT_INT:
            case BCVM_OP_PRINT_CHAR:
                if (state->sp == 0U
                    || uvm_syscall(opcode == BCVM_OP_PRINT_INT
                            ? USER_SYSCALL_DRIVER_CONSOLE_WRITE_INT
                            : USER_SYSCALL_DRIVER_CONSOLE_WRITE_CHAR,
                        (uint32_t)state->stack[--state->sp], 0U, 0U, 0U) < 0) {
                    goto failed;
                }
                break;
            case BCVM_OP_PRINT_FLOAT:
                if (state->sp == 0U
                    || uvm_print_fixed(state->stack[--state->sp]) != 0) {
                    goto failed;
                }
                break;
            case BCVM_OP_PRINT_NL:
                if (uvm_syscall(USER_SYSCALL_DRIVER_CONSOLE_WRITE_NL, 0U, 0U, 0U, 0U) < 0) {
                    goto failed;
                }
                break;
            case BCVM_OP_STR_LEN: {
                const char* text;
                uint16_t length;
                if (state->sp == 0U
                    || uvm_resolve_string(state, (const uint8_t*)(uintptr_t)data_address,
                        data_size, (uint32_t)state->stack[state->sp - 1U], &text, &length) != 0) {
                    goto failed;
                }
                state->stack[state->sp - 1U] = (int32_t)length;
                break;
            }
            case BCVM_OP_STR_EQ: {
                const char* right_text;
                const char* left_text;
                uint16_t right_length;
                uint16_t left_length;
                int equal = 1;
                if (state->sp < 2U
                    || uvm_resolve_string(state, (const uint8_t*)(uintptr_t)data_address,
                        data_size, (uint32_t)state->stack[state->sp - 1U],
                        &right_text, &right_length) != 0
                    || uvm_resolve_string(state, (const uint8_t*)(uintptr_t)data_address,
                        data_size, (uint32_t)state->stack[state->sp - 2U],
                        &left_text, &left_length) != 0) {
                    goto failed;
                }
                if (left_length != right_length) equal = 0;
                for (uint16_t index = 0U; equal != 0 && index < left_length; ++index) {
                    if (left_text[index] != right_text[index]) equal = 0;
                }
                state->sp -= 2U;
                if (uvm_push(state, equal) != 0) goto failed;
                break;
            }
            case BCVM_OP_ARR_NEW: {
                uint32_t handle;
                uint16_t length = uvm_read_u16((const uint8_t*)(uintptr_t)code_address,
                    code_size, state->pc, &valid);
                if (!valid || uvm_new_array(state, length, &handle) != 0) {
                    goto failed;
                }
                state->pc += 2U;
                if (uvm_push(state, (int32_t)handle) != 0) goto failed;
                break;
            }
            case BCVM_OP_ARR_GET:
            case BCVM_OP_ARR_SET: {
                uint32_t handle;
                uint32_t index;
                uint16_t length;
                if (state->sp < (opcode == BCVM_OP_ARR_GET ? 2U : 3U)) goto failed;
                if (opcode == BCVM_OP_ARR_GET) {
                    index = (uint32_t)state->stack[--state->sp];
                    handle = (uint32_t)state->stack[--state->sp];
                } else {
                    int32_t value = state->stack[--state->sp];
                    index = (uint32_t)state->stack[--state->sp];
                    handle = (uint32_t)state->stack[--state->sp];
                    if (handle == 0U || handle > USER_VM_HEAP_ARRAY_MAX) goto failed;
                    length = state->heap_array_length[handle - 1U];
                    if (state->heap_array_used[handle - 1U] == 0U || index >= length) goto failed;
                    state->heap_array_data[state->heap_array_offset[handle - 1U] + index] = value;
                    break;
                }
                if (handle == 0U || handle > USER_VM_HEAP_ARRAY_MAX) goto failed;
                length = state->heap_array_length[handle - 1U];
                if (state->heap_array_used[handle - 1U] == 0U || index >= length
                    || uvm_push(state, state->heap_array_data[state->heap_array_offset[handle - 1U] + index]) != 0) {
                    goto failed;
                }
                break;
            }
            case BCVM_OP_DRIVER_SERIAL_READ:
                if (uvm_push(state, uvm_syscall(USER_SYSCALL_DRIVER_SERIAL_READ, 0U, 0U, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_PORT_READ8:
            case BCVM_OP_DRIVER_MMIO_READ32:
                if (state->sp == 0U) goto failed;
                state->stack[state->sp - 1U] = uvm_syscall(
                    opcode == BCVM_OP_DRIVER_PORT_READ8
                        ? USER_SYSCALL_DRIVER_PORT_READ8 : USER_SYSCALL_DRIVER_MMIO_READ32,
                    (uint32_t)state->stack[state->sp - 1U], 0U, 0U, 0U);
                break;
            case BCVM_OP_DRIVER_SHOULD_STOP:
                if (uvm_push(state, uvm_syscall(USER_SYSCALL_DRIVER_SHOULD_STOP, 0U, 0U, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_IRQ_READ:
                if (uvm_push(state, uvm_syscall(USER_SYSCALL_DRIVER_IRQ_READ,
                        0U, 0U, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_IRQ_DROPPED:
                if (uvm_push(state, uvm_syscall(USER_SYSCALL_DRIVER_IRQ_DROPPED,
                        0U, 0U, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_IRQ_ACK:
                if (uvm_push(state, uvm_syscall(USER_SYSCALL_DRIVER_IRQ_ACK,
                        0U, 0U, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_PCI_SUBSCRIBE_IRQ:
                if (state->sp == 0U) goto failed;
                state->stack[state->sp - 1U] = uvm_syscall(
                    USER_SYSCALL_DRIVER_PCI_SUBSCRIBE_IRQ,
                    (uint32_t)state->stack[state->sp - 1U], 0U, 0U, 0U);
                break;
            case BCVM_OP_DRIVER_PCI_FIND:
            case BCVM_OP_DRIVER_PCI_READ_CONFIG32:
            case BCVM_OP_DRIVER_PCI_READ_BAR32:
            case BCVM_OP_DRIVER_PCI_MMIO_READ32:
            case BCVM_OP_DRIVER_DMA_ALLOC_BELOW:
            case BCVM_OP_DRIVER_PCI_ENABLE:
            case BCVM_OP_DRIVER_PCI_DISABLE:
            case BCVM_OP_DRIVER_PCI_CLAIM_IO_BAR:
            case BCVM_OP_DRIVER_PCI_CLAIM_MMIO_BAR:
                if (state->sp < 2U) goto failed;
                right = state->stack[--state->sp];
                left = state->stack[--state->sp];
                if (uvm_push(state, uvm_syscall(
                        opcode == BCVM_OP_DRIVER_PCI_FIND ? USER_SYSCALL_DRIVER_PCI_FIND
                        : opcode == BCVM_OP_DRIVER_PCI_READ_CONFIG32 ? USER_SYSCALL_DRIVER_PCI_READ_CONFIG32
                        : opcode == BCVM_OP_DRIVER_PCI_READ_BAR32 ? USER_SYSCALL_DRIVER_PCI_READ_BAR32
                        : opcode == BCVM_OP_DRIVER_PCI_MMIO_READ32 ? USER_SYSCALL_DRIVER_PCI_MMIO_READ32
                        : opcode == BCVM_OP_DRIVER_DMA_ALLOC_BELOW ? USER_SYSCALL_DRIVER_DMA_ALLOC_BELOW
                        : opcode == BCVM_OP_DRIVER_PCI_ENABLE ? USER_SYSCALL_DRIVER_PCI_ENABLE
                        : opcode == BCVM_OP_DRIVER_PCI_DISABLE ? USER_SYSCALL_DRIVER_PCI_DISABLE
                        : opcode == BCVM_OP_DRIVER_PCI_CLAIM_MMIO_BAR ? USER_SYSCALL_DRIVER_PCI_CLAIM_MMIO_BAR
                        : USER_SYSCALL_DRIVER_PCI_CLAIM_IO_BAR,
                        (uint32_t)left, (uint32_t)right, 0U, 0U)) != 0) goto failed;
                break;
            case BCVM_OP_DRIVER_PCI_CLAIM:
            case BCVM_OP_DRIVER_DMA_ALLOC:
            case BCVM_OP_DRIVER_DMA_PHYSICAL:
            case BCVM_OP_DRIVER_DMA_FREE:
            case BCVM_OP_DRIVER_STATE_READ:
                if (state->sp == 0U) goto failed;
                state->stack[state->sp - 1U] = uvm_syscall(
                    opcode == BCVM_OP_DRIVER_PCI_CLAIM ? USER_SYSCALL_DRIVER_PCI_CLAIM
                    : opcode == BCVM_OP_DRIVER_DMA_ALLOC ? USER_SYSCALL_DRIVER_DMA_ALLOC
                    : opcode == BCVM_OP_DRIVER_DMA_PHYSICAL ? USER_SYSCALL_DRIVER_DMA_PHYSICAL
                    : opcode == BCVM_OP_DRIVER_DMA_FREE ? USER_SYSCALL_DRIVER_DMA_FREE
                    : USER_SYSCALL_DRIVER_STATE_READ,
                    (uint32_t)state->stack[state->sp - 1U], 0U, 0U, 0U);
                break;
            case BCVM_OP_DRIVER_PORT_WRITE8:
            case BCVM_OP_DRIVER_MMIO_WRITE32:
            case BCVM_OP_DRIVER_STATE_WRITE:
            case BCVM_OP_DRIVER_DMA_READ:
            case BCVM_OP_DRIVER_PCI_IO_READ:
            case BCVM_OP_DRIVER_DMA_WRITE:
            case BCVM_OP_DRIVER_PCI_IO_WRITE:
            case BCVM_OP_DRIVER_PCI_MMIO_WRITE32: {
                uint32_t argument_count = opcode == BCVM_OP_DRIVER_STATE_WRITE
                    || opcode == BCVM_OP_DRIVER_PORT_WRITE8
                    || opcode == BCVM_OP_DRIVER_MMIO_WRITE32 ? 2U
                    : opcode == BCVM_OP_DRIVER_DMA_READ
                    || opcode == BCVM_OP_DRIVER_PCI_IO_READ
                    || opcode == BCVM_OP_DRIVER_PCI_MMIO_WRITE32 ? 3U : 4U;
                uint32_t args[4] = {0U, 0U, 0U, 0U};
                if (state->sp < argument_count) goto failed;
                for (uint32_t index = 0U; index < argument_count; ++index) {
                    args[argument_count - 1U - index] = (uint32_t)state->stack[--state->sp];
                }
                {
                    uint32_t syscall_number =
                        opcode == BCVM_OP_DRIVER_PORT_WRITE8 ? USER_SYSCALL_DRIVER_PORT_WRITE8
                        : opcode == BCVM_OP_DRIVER_MMIO_WRITE32 ? USER_SYSCALL_DRIVER_MMIO_WRITE32
                        : opcode == BCVM_OP_DRIVER_STATE_WRITE ? USER_SYSCALL_DRIVER_STATE_WRITE
                        : opcode == BCVM_OP_DRIVER_DMA_READ ? USER_SYSCALL_DRIVER_DMA_READ
                        : opcode == BCVM_OP_DRIVER_PCI_IO_READ ? USER_SYSCALL_DRIVER_PCI_IO_READ
                        : opcode == BCVM_OP_DRIVER_DMA_WRITE ? USER_SYSCALL_DRIVER_DMA_WRITE
                        : opcode == BCVM_OP_DRIVER_PCI_IO_WRITE ? USER_SYSCALL_DRIVER_PCI_IO_WRITE
                        : USER_SYSCALL_DRIVER_PCI_MMIO_WRITE32;
                    int32_t result = uvm_syscall(syscall_number, args[0], args[1],
                        args[2], args[3]);
                    if (uvm_push(state, result) != 0) goto failed;
                }
                break;
            }
            case BCVM_OP_POP:
                if (state->sp == 0U) goto failed;
                --state->sp;
                break;
            case BCVM_OP_RET:
                if (state->sp == 0U) goto failed;
                if (state->call_depth == 0U) {
                    state->return_value = state->stack[state->sp - 1U];
                    state->active_call = 0U;
                    return 0;
                }
                operand = (uint32_t)state->stack[--state->sp];
                --state->call_depth;
                if (uvm_push(state, (int32_t)operand) != 0) goto failed;
                state->pc = state->call_return_pc[state->call_depth];
                break;
            case BCVM_OP_HALT:
                state->return_value = 0;
                state->active_call = 0U;
                return 0;
            default:
                goto failed;
        }

        if ((slice_steps % USER_VM_PREEMPT_CHECK_INTERVAL) == 0U) {
            int32_t preempt = uvm_syscall(
                USER_SYSCALL_DRIVER_PREEMPT_CHECK, 0U, 0U, 0U, 0U);
            if (preempt < 0) goto failed;
            if (preempt != 0) return BCVM_DRIVER_SLICE_YIELDED;
        }
    }

failed:
    state->active_call = 0U;
    return BCVM_DRIVER_SLICE_FAILED;
}

USER_VM_CODE __attribute__((noreturn)) void user_driver_vm_entry(
    const user_vm_launch_t* launch)
{
    int result = uvm_run(launch);
    __asm__ volatile ("int $0x80"
        :
        : "a"(USER_SYSCALL_EXIT), "b"(result)
        : "memory", "cc");
    for (;;) {
        __asm__ volatile ("ud2");
    }
}

static uint32_t align_page(uint32_t value)
{
    return (value + 4095U) & 0xFFFFF000U;
}

static uint16_t kernel_read_u16(const uint8_t* bytes, uint32_t offset)
{
    return (uint16_t)(bytes[offset] | ((uint16_t)bytes[offset + 1U] << 8));
}

static uint32_t kernel_read_u32(const uint8_t* bytes, uint32_t offset)
{
    return (uint32_t)bytes[offset]
        | ((uint32_t)bytes[offset + 1U] << 8)
        | ((uint32_t)bytes[offset + 2U] << 16)
        | ((uint32_t)bytes[offset + 3U] << 24);
}

static int validate_driver_image(const uint8_t* image, uint32_t image_size)
{
    uint16_t version;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t pc = 0U;

    if (image == 0 || image_size < BCVM_IMAGE_HEADER_SIZE
        || kernel_read_u32(image, 0U) != BCVM_MAGIC) {
        return -1;
    }
    version = kernel_read_u16(image, 4U);
    code_size = kernel_read_u32(image, 8U);
    data_size = kernel_read_u32(image, 12U);
    if (version < BCVM_MIN_COMPATIBLE_VERSION || version > BCVM_VERSION
        || code_size == 0U || code_size > image_size - BCVM_IMAGE_HEADER_SIZE
        || data_size > image_size - BCVM_IMAGE_HEADER_SIZE - code_size
        || kernel_read_u32(image, 16U) >= code_size) {
        return -1;
    }

    while (pc < code_size) {
        uint8_t opcode = image[BCVM_IMAGE_HEADER_SIZE + pc++];
        uint32_t immediate = 0U;

        switch (opcode) {
            case BCVM_OP_PUSH_I32:
            case BCVM_OP_JMP:
            case BCVM_OP_JMP_IF_ZERO:
            case BCVM_OP_CALL:
                immediate = 4U;
                break;
            case BCVM_OP_CALL_ARGS:
                immediate = 5U;
                break;
            case BCVM_OP_LOAD_LOCAL:
            case BCVM_OP_STORE_LOCAL:
            case BCVM_OP_ARR_NEW:
                immediate = 2U;
                break;
            case BCVM_OP_PRINT_STR:
            case BCVM_OP_PRINT_COLOR_STR:
                if (pc > code_size || code_size - pc < 4U) return -1;
                if ((uint32_t)kernel_read_u16(image,
                        BCVM_IMAGE_HEADER_SIZE + pc)
                        + kernel_read_u16(image, BCVM_IMAGE_HEADER_SIZE + pc + 2U)
                    > data_size) {
                    return -1;
                }
                immediate = 4U;
                break;
            case BCVM_OP_ADD:
            case BCVM_OP_SUB:
            case BCVM_OP_MUL:
            case BCVM_OP_DIV:
            case BCVM_OP_MOD:
            case BCVM_OP_NEG:
            case BCVM_OP_BIT_NOT:
            case BCVM_OP_EQ:
            case BCVM_OP_NE:
            case BCVM_OP_LT:
            case BCVM_OP_LE:
            case BCVM_OP_GT:
            case BCVM_OP_GE:
            case BCVM_OP_FMUL:
            case BCVM_OP_FDIV:
            case BCVM_OP_BIT_AND:
            case BCVM_OP_BIT_OR:
            case BCVM_OP_BIT_XOR:
            case BCVM_OP_SHIFT_LEFT:
            case BCVM_OP_SHIFT_RIGHT:
            case BCVM_OP_PRINT_INT:
            case BCVM_OP_PRINT_FLOAT:
            case BCVM_OP_PRINT_STR_VAL:
            case BCVM_OP_PRINT_COLOR_STR_VAL:
            case BCVM_OP_PRINT_NL:
            case BCVM_OP_PRINT_CHAR:
            case BCVM_OP_STR_LEN:
            case BCVM_OP_STR_EQ:
            case BCVM_OP_ARR_GET:
            case BCVM_OP_ARR_SET:
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
            case BCVM_OP_DRIVER_PCI_DISABLE:
            case BCVM_OP_DRIVER_PCI_CLAIM_IO_BAR:
            case BCVM_OP_DRIVER_PCI_IO_READ:
            case BCVM_OP_DRIVER_PCI_IO_WRITE:
            case BCVM_OP_DRIVER_DMA_ALLOC:
            case BCVM_OP_DRIVER_DMA_ALLOC_BELOW:
            case BCVM_OP_DRIVER_DMA_PHYSICAL:
            case BCVM_OP_DRIVER_DMA_READ:
            case BCVM_OP_DRIVER_DMA_WRITE:
            case BCVM_OP_DRIVER_DMA_FREE:
            case BCVM_OP_DRIVER_PCI_CLAIM_MMIO_BAR:
            case BCVM_OP_DRIVER_PCI_MMIO_READ32:
            case BCVM_OP_DRIVER_PCI_MMIO_WRITE32:
            case BCVM_OP_DRIVER_STATE_READ:
            case BCVM_OP_DRIVER_STATE_WRITE:
            case BCVM_OP_DRIVER_PCI_SUBSCRIBE_IRQ:
            case BCVM_OP_DRIVER_IRQ_READ:
            case BCVM_OP_DRIVER_IRQ_DROPPED:
            case BCVM_OP_DRIVER_IRQ_ACK:
            case BCVM_OP_POP:
            case BCVM_OP_RET:
            case BCVM_OP_HALT:
                break;
            default:
                return -1;
        }

        if (pc > code_size || immediate > code_size - pc) return -1;
        if (opcode == BCVM_OP_LOAD_LOCAL || opcode == BCVM_OP_STORE_LOCAL) {
            if (kernel_read_u16(image, BCVM_IMAGE_HEADER_SIZE + pc) >= USER_VM_LOCALS_MAX) {
                return -1;
            }
        }
        if (opcode == BCVM_OP_ARR_NEW) {
            uint16_t length = kernel_read_u16(image, BCVM_IMAGE_HEADER_SIZE + pc);
            if (length == 0U || length > USER_VM_HEAP_ARRAY_CELLS) return -1;
        }
        if (opcode == BCVM_OP_JMP || opcode == BCVM_OP_JMP_IF_ZERO
            || opcode == BCVM_OP_CALL || opcode == BCVM_OP_CALL_ARGS) {
            uint32_t target = kernel_read_u32(image, BCVM_IMAGE_HEADER_SIZE + pc);
            if (target >= code_size) return -1;
        }
        pc += immediate;
    }
    return 0;
}

static void zero_bytes(void* destination, uint32_t length)
{
    uint8_t* bytes = (uint8_t*)destination;
    for (uint32_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static int map_physical_pages(const paging_user_space_t* space,
    uint32_t virtual_address, uint32_t physical_address, uint32_t page_count,
    uint32_t flags)
{
    for (uint32_t index = 0U; index < page_count; ++index) {
        if (paging_user_space_map(space, virtual_address + index * 4096U,
                physical_address + index * 4096U, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static void release_runtime(user_driver_runtime_t* runtime)
{
    if (runtime->allocated != 0U) {
        paging_kernel_space_activate();
        paging_user_space_destroy(&runtime->address_space);
    }
    if (runtime->image_pages != 0) {
        pmm_free_pages(runtime->image_pages, runtime->image_page_count);
    }
    if (runtime->state_pages != 0) {
        pmm_free_pages(runtime->state_pages, runtime->state_page_count);
    }
    if (runtime->user_stack_pages != 0) {
        pmm_free_pages(runtime->user_stack_pages, runtime->user_stack_page_count);
    }
    if (runtime->kernel_stack_pages != 0) {
        pmm_free_pages(runtime->kernel_stack_pages, USER_VM_KERNEL_STACK_PAGES);
    }
    *runtime = (user_driver_runtime_t){0};
}

static int create_runtime(user_driver_runtime_t* runtime,
    const uint8_t* image, uint32_t image_size)
{
    uint32_t section_start = (uint32_t)(uintptr_t)&__user_vm_start;
    uint32_t section_end = (uint32_t)(uintptr_t)&__user_vm_end;
    uint32_t section_load = (uint32_t)(uintptr_t)&__user_vm_load_start;
    uint32_t image_bytes = align_page(image_size);
    uint32_t launch_bytes = align_page((uint32_t)sizeof(user_vm_launch_t));
    uint32_t state_bytes = launch_bytes
        + 2U * align_page((uint32_t)sizeof(user_vm_state_t));
    uint32_t code_bytes;
    uint32_t image_page_count;
    uint32_t state_page_count;
    uint32_t image_user_address;
    uint32_t state_user_address;
    uint32_t stack_user_address;

    if (!user_mode_isolation_available() || image == 0
        || validate_driver_image(image, image_size) != 0
        || section_start != PAGING_USER_BASE
        || section_end <= section_start
        || section_end > PAGING_USER_BASE + PAGING_USER_SIZE) {
        return -1;
    }
    code_bytes = section_end - section_start;
    runtime->section_page_count = code_bytes / 4096U;
    image_page_count = image_bytes / 4096U;
    state_page_count = state_bytes / 4096U;
    image_user_address = section_end;
    state_user_address = image_user_address + image_bytes;
    stack_user_address = PAGING_USER_BASE + PAGING_USER_SIZE
        - USER_VM_USER_STACK_PAGES * 4096U;
    if (state_user_address > stack_user_address
        || state_bytes > stack_user_address - state_user_address) {
        return -1;
    }

    if (paging_user_space_create(&runtime->address_space) != 0) {
        return -1;
    }
    runtime->allocated = 1U;
    runtime->image_page_count = image_page_count;
    runtime->state_page_count = state_page_count;
    runtime->user_stack_page_count = USER_VM_USER_STACK_PAGES;
    runtime->image_user_address = image_user_address;
    runtime->state_user_address = state_user_address;
    runtime->vm_state_user_address = state_user_address + launch_bytes;
    runtime->service_vm_state_user_address = runtime->vm_state_user_address
        + align_page((uint32_t)sizeof(user_vm_state_t));
    runtime->user_stack_top = stack_user_address
        + USER_VM_USER_STACK_PAGES * 4096U;

    runtime->image_pages = pmm_alloc_pages_below(image_page_count,
        USER_VM_PHYSICAL_LIMIT);
    runtime->state_pages = pmm_alloc_pages_below(state_page_count,
        USER_VM_PHYSICAL_LIMIT);
    runtime->user_stack_pages = pmm_alloc_pages_below(USER_VM_USER_STACK_PAGES,
        USER_VM_PHYSICAL_LIMIT);
    runtime->kernel_stack_pages = pmm_alloc_pages_below(USER_VM_KERNEL_STACK_PAGES,
        USER_VM_PHYSICAL_LIMIT);
    if (runtime->image_pages == 0 || runtime->state_pages == 0
        || runtime->user_stack_pages == 0 || runtime->kernel_stack_pages == 0) {
        release_runtime(runtime);
        return -1;
    }
    zero_bytes(runtime->state_pages, state_page_count * 4096U);
    for (uint32_t index = 0U; index < image_size; ++index) {
        ((uint8_t*)runtime->image_pages)[index] = image[index];
    }

    if (map_physical_pages(&runtime->address_space, section_start,
            section_load, runtime->section_page_count, 0U) != 0
        || map_physical_pages(&runtime->address_space, image_user_address,
            (uint32_t)(uintptr_t)runtime->image_pages, image_page_count, 0U) != 0
        || map_physical_pages(&runtime->address_space, state_user_address,
            (uint32_t)(uintptr_t)runtime->state_pages, state_page_count,
            PAGING_FLAG_READ_WRITE) != 0
        || map_physical_pages(&runtime->address_space, stack_user_address,
            (uint32_t)(uintptr_t)runtime->user_stack_pages,
            USER_VM_USER_STACK_PAGES, PAGING_FLAG_READ_WRITE) != 0) {
        release_runtime(runtime);
        return -1;
    }
    runtime->kernel_stack_top = (uint32_t)(uintptr_t)runtime->kernel_stack_pages
        + USER_VM_KERNEL_STACK_PAGES * 4096U;
    runtime->mode = (user_mode_context_t){0};
    runtime->mode.driver_services_enabled = 1U;
    runtime->source_image = image;
    runtime->source_image_size = image_size;
    return 0;
}

static int user_driver_vm_run(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int resume,
    const int32_t* arguments, uint32_t argument_count, int service)
{
    user_driver_runtime_t* runtime;
    user_vm_launch_t launch;
    uint32_t stack_args[2];
    int status;

    if (runtime_handle >= DRIVER_RUNTIME_MAX || image == 0
        || (entry_offset != 0xFFFFFFFFU && entry_offset >= image_size)) {
        return BCVM_DRIVER_SLICE_FAILED;
    }
    runtime = &user_driver_runtimes[runtime_handle];
    if (runtime->allocated == 0U) {
        if (resume != 0 || create_runtime(runtime, image, image_size) != 0) {
            return BCVM_DRIVER_SLICE_FAILED;
        }
    }
    if (runtime->source_image != image || runtime->source_image_size != image_size
        || runtime->image_pages == 0
        || runtime->image_user_address == 0U) {
        return BCVM_DRIVER_SLICE_FAILED;
    }

    launch.image_address = runtime->image_user_address;
    launch.image_size = image_size;
    launch.entry_offset = entry_offset;
    launch.state_address = service != 0
        ? runtime->service_vm_state_user_address
        : runtime->vm_state_user_address;
    launch.resume = resume != 0 ? 1U : 0U;
    launch.step_budget = USER_VM_SLICE_STEPS;
    launch.argument_count = argument_count;
    launch.arguments[0] = argument_count > 0U ? arguments[0] : 0;
    launch.arguments[1] = argument_count > 1U ? arguments[1] : 0;
    launch.arguments[2] = argument_count > 2U ? arguments[2] : 0;
    runtime->mode.driver_runtime_handle = (uint8_t)runtime_handle;
    runtime->mode.stop_requested = 0U;
    stack_args[0] = 0U;
    stack_args[1] = runtime->state_user_address;

    if (paging_user_space_activate(&runtime->address_space) != 0
        || paging_user_space_copy_to(&runtime->address_space,
            runtime->state_user_address, &launch, sizeof(launch)) != 0
        || paging_user_space_copy_to(&runtime->address_space,
            runtime->user_stack_top - sizeof(stack_args), stack_args,
            sizeof(stack_args)) != 0) {
        ERROR_LOG("ring-3 driver VM could not activate or prepare user memory");
        paging_kernel_space_activate();
        return BCVM_DRIVER_SLICE_FAILED;
    }

    status = user_mode_enter(&runtime->mode, &runtime->address_space,
        (uint32_t)(uintptr_t)user_driver_vm_entry,
        runtime->user_stack_top - sizeof(stack_args),
        runtime->kernel_stack_top);
    if (status == -2) {
        ERROR_LOG("ring-3 driver VM trapped on an invalid memory or CPU access");
    }
    if (status == 0 || status == BCVM_DRIVER_SLICE_YIELDED) {
        const user_vm_state_t* state = (const user_vm_state_t*)
            ((const uint8_t*)runtime->state_pages
                + align_page((uint32_t)sizeof(user_vm_launch_t))
                + (service != 0
                    ? align_page((uint32_t)sizeof(user_vm_state_t)) : 0U));
        runtime->return_value = state->return_value;
    }
    paging_kernel_space_activate();
    return status;
}

int user_driver_vm_slice(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int resume)
{
    return user_driver_vm_run(image, image_size, entry_offset, runtime_handle,
        resume, 0, 0U, 0);
}

int user_driver_vm_service(const uint8_t* image, uint32_t image_size,
    uint32_t entry_offset, uint32_t runtime_handle, int32_t operation,
    int32_t argument0, int32_t argument1, int resume)
{
    const int32_t arguments[3] = {operation, argument0, argument1};
    return user_driver_vm_run(image, image_size, entry_offset, runtime_handle,
        resume, arguments, 3U, 1);
}

void user_driver_vm_reset(uint32_t runtime_handle)
{
    user_driver_runtime_t* runtime;
    if (runtime_handle >= DRIVER_RUNTIME_MAX) return;
    runtime = &user_driver_runtimes[runtime_handle];
    if (runtime->state_pages != 0) {
        zero_bytes((uint8_t*)runtime->state_pages
            + align_page((uint32_t)sizeof(user_vm_launch_t)),
            (uint32_t)sizeof(user_vm_state_t));
        zero_bytes((uint8_t*)runtime->state_pages
            + align_page((uint32_t)sizeof(user_vm_launch_t))
            + align_page((uint32_t)sizeof(user_vm_state_t)),
            (uint32_t)sizeof(user_vm_state_t));
    }
    runtime->return_value = 0;
    runtime->mode.stop_requested = 0U;
}

void user_driver_vm_destroy(uint32_t runtime_handle)
{
    if (runtime_handle < DRIVER_RUNTIME_MAX) {
        release_runtime(&user_driver_runtimes[runtime_handle]);
    }
}

int32_t user_driver_vm_result(uint32_t runtime_handle)
{
    if (runtime_handle >= DRIVER_RUNTIME_MAX) return -1;
    return user_driver_runtimes[runtime_handle].return_value;
}

int user_driver_vm_stop_requested(uint32_t runtime_handle)
{
    if (runtime_handle >= DRIVER_RUNTIME_MAX) return 0;
    return user_driver_runtimes[runtime_handle].mode.stop_requested != 0U;
}

int user_driver_vm_self_test(void)
{
    static const uint8_t image[] = {
        'B', 'C', 'V', 'M', 3U, 0U, 0U, 0U,
        32U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U,
        BCVM_OP_PUSH_I32, 0U, 0U, 0U, 0U,
        BCVM_OP_PUSH_I32, 42U, 0U, 0U, 0U,
        BCVM_OP_DRIVER_STATE_WRITE,
        BCVM_OP_POP,
        BCVM_OP_PUSH_I32, 0U, 0U, 0U, 0U,
        BCVM_OP_DRIVER_PCI_SUBSCRIBE_IRQ,
        BCVM_OP_POP,
        BCVM_OP_DRIVER_IRQ_READ,
        BCVM_OP_POP,
        BCVM_OP_DRIVER_IRQ_DROPPED,
        BCVM_OP_POP,
        BCVM_OP_DRIVER_IRQ_ACK,
        BCVM_OP_POP,
        BCVM_OP_PUSH_I32, 0U, 0U, 0U, 0U,
        BCVM_OP_DRIVER_STATE_READ,
        BCVM_OP_RET
    };
    static const uint8_t yielding_image[] = {
        'B', 'C', 'V', 'M', 3U, 0U, 0U, 0U,
        11U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U,
        BCVM_OP_PUSH_I32, 1U, 0U, 0U, 0U,
        BCVM_OP_POP,
        BCVM_OP_JMP, 0U, 0U, 0U, 0U
    };
    int runtime_handle;
    int status = BCVM_DRIVER_SLICE_FAILED;

    driver_vm_self_test_passed = 0;
    if (!user_mode_isolation_available()) {
        ERROR_LOG("ring-3 bytecode self-test skipped: base isolation unavailable");
        return -1;
    }
    runtime_handle = driver_runtime_create();
    if (runtime_handle < 0) {
        ERROR_LOG("ring-3 bytecode self-test could not allocate a driver context");
        return -1;
    }
    status = user_driver_vm_slice(image, sizeof(image), 0U,
        (uint32_t)runtime_handle, 0);
    if (status != 0) {
        ERROR_LOG("ring-3 bytecode interpreter returned an execution error");
    } else if (user_driver_vm_result((uint32_t)runtime_handle) != 42) {
        ERROR_LOG("ring-3 bytecode interpreter returned the wrong result");
    }
    if (status == 0 && user_driver_vm_result((uint32_t)runtime_handle) == 42) {
        user_driver_vm_destroy((uint32_t)runtime_handle);
        status = user_driver_vm_slice(yielding_image, sizeof(yielding_image),
            0U, (uint32_t)runtime_handle, 0);
        if (status == BCVM_DRIVER_SLICE_YIELDED) {
            status = user_driver_vm_slice(yielding_image,
                sizeof(yielding_image), 0U, (uint32_t)runtime_handle, 1);
        }
        if (status == BCVM_DRIVER_SLICE_YIELDED) {
            driver_vm_self_test_passed = 1;
        } else {
            ERROR_LOG("ring-3 bytecode interpreter failed bounded resume test");
        }
    }
    user_driver_vm_destroy((uint32_t)runtime_handle);
    driver_runtime_destroy((uint32_t)runtime_handle);
    return driver_vm_self_test_passed ? 0 : -1;
}

int user_driver_vm_isolation_available(void)
{
    return driver_vm_self_test_passed;
}
