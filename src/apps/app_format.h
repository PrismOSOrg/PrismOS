#ifndef PRISMOS_APPS_APP_FORMAT_H
#define PRISMOS_APPS_APP_FORMAT_H

#include <stdint.h>

#define PRISM_APP_MAGIC 0x50524953U /* 'PRIS' */
#define PRISM_APP_FORMAT_VERSION 1U
#define PRISM_APP_FLAG_DRIVER 0x0001U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_offset;
    uint32_t image_size;
    uint32_t reserved;
} prism_app_header_t;

#define PRISM_APP_HEADER_SIZE 20U

#define BCVM_MAGIC 0x4D564342U /* 'BCVM' */
#define BCVM_VERSION 2U
#define BCVM_IMAGE_HEADER_SIZE 20U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t entry_offset;
} bcvm_image_header_t;

enum {
    BCVM_OP_PUSH_I32 = 0x01,
    BCVM_OP_LOAD_LOCAL = 0x02,
    BCVM_OP_STORE_LOCAL = 0x03,
    BCVM_OP_ADD = 0x04,
    BCVM_OP_SUB = 0x05,
    BCVM_OP_MUL = 0x06,
    BCVM_OP_DIV = 0x07,
    BCVM_OP_PRINT_STR = 0x08,
    BCVM_OP_PRINT_INT = 0x09,
    BCVM_OP_PRINT_NL = 0x0A,
    BCVM_OP_POP = 0x0B,
    BCVM_OP_RET = 0x0C,
    BCVM_OP_HALT = 0x0D,
    BCVM_OP_NEG = 0x0E,
    BCVM_OP_JMP = 0x0F,
    BCVM_OP_JMP_IF_ZERO = 0x10,
    BCVM_OP_EQ = 0x11,
    BCVM_OP_NE = 0x12,
    BCVM_OP_LT = 0x13,
    BCVM_OP_LE = 0x14,
    BCVM_OP_GT = 0x15,
    BCVM_OP_GE = 0x16,
    BCVM_OP_CALL = 0x17,
    BCVM_OP_CALL_ARGS = 0x18,
    BCVM_OP_READ_INT = 0x19,
    BCVM_OP_PRINT_COLOR_STR = 0x1A,
    BCVM_OP_READ_TEXT = 0x1B,
    BCVM_OP_PRINT_INPUT = 0x1C,
    BCVM_OP_INPUT_LEN = 0x1D,
    BCVM_OP_INPUT_EQ = 0x1E,
    BCVM_OP_PRINT_STR_VAL = 0x1F,
    BCVM_OP_STR_LEN = 0x20,
    BCVM_OP_STR_EQ = 0x21,
    BCVM_OP_PRINT_COLOR_STR_VAL = 0x22,
    BCVM_OP_ARR_NEW = 0x23,
    BCVM_OP_ARR_GET = 0x24,
    BCVM_OP_ARR_SET = 0x25,
    BCVM_OP_FILE_READ = 0x26,
    BCVM_OP_FILE_WRITE = 0x27,
    BCVM_OP_FILE_APPEND = 0x28,
    BCVM_OP_FILE_EXISTS = 0x29,
    BCVM_OP_MOD = 0x2A,
    BCVM_OP_SET_FULLSCREEN = 0x2B,
    BCVM_OP_APP_SHOULD_QUIT = 0x2C,
    BCVM_OP_APP_EXIT = 0x2D,
    BCVM_OP_DRAW_PIXEL = 0x2E,
    BCVM_OP_PRINT_CHAR = 0x2F,
    BCVM_OP_FMUL = 0x30,
    BCVM_OP_FDIV = 0x31,
    BCVM_OP_PRINT_FLOAT = 0x32,
    BCVM_OP_DRIVER_SERIAL_WRITE = 0x33,
    BCVM_OP_DRIVER_SERIAL_READ = 0x34,
    BCVM_OP_DRIVER_PORT_READ8 = 0x35,
    BCVM_OP_DRIVER_PORT_WRITE8 = 0x36,
    BCVM_OP_DRIVER_MMIO_READ32 = 0x37,
    BCVM_OP_DRIVER_MMIO_WRITE32 = 0x38,
    BCVM_OP_DRIVER_SHOULD_STOP = 0x39,
};

#endif
