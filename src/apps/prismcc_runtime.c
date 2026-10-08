#include "apps/prismcc_runtime.h"

#include "apps/app_format.h"
#include "debug/log.h"
#include "filesystem/vfs.h"

#define PRISMCC_MAX_SOURCE_SIZE (32U * 1024U)
#define PRISMCC_MAX_CODE_SIZE (48U * 1024U)
#define PRISMCC_MAX_DATA_SIZE (16U * 1024U)
#define PRISMCC_MAX_LOCALS 512U
#define PRISMCC_MAX_TOKEN_TEXT 64U
#define PRISMCC_MAX_FUNCTIONS 64U
#define PRISMCC_MAX_CALL_PATCHES 256U
#define PRISMCC_MAX_PARAMS 255U
#define PRISMCC_MAX_STRUCTS 32U
#define PRISMCC_MAX_STRUCT_FIELDS 16U
#define PRISMCC_MAX_INCLUDE_DEPTH 8U
#define PRISMCC_MAX_PATH 128U
#define PRISMCC_FLOAT_SCALE 1000

typedef enum {
    TOK_EOF = 0,
    TOK_INT,
    TOK_INT8,
    TOK_INT16,
    TOK_INT32,
    TOK_UINT8,
    TOK_UINT16,
    TOK_UINT32,
    TOK_VOID,
    TOK_FLOAT_TYPE,
    TOK_CHAR_TYPE,
    TOK_MAIN,
    TOK_RETURN,
    TOK_PRINT,
    TOK_PRINT_INT,
    TOK_PRINT_CHAR,
    TOK_PRINT_COLOR,
    TOK_READ_TEXT,
    TOK_PRINT_INPUT,
    TOK_INPUT_INT,
    TOK_INPUT_LEN,
    TOK_INPUT_EQ,
    TOK_STRING_LEN,
    TOK_STRING_EQ,
    TOK_FILE_READ,
    TOK_FILE_WRITE,
    TOK_FILE_APPEND,
    TOK_FILE_EXISTS,
    TOK_SET_FULLSCREEN,
    TOK_APP_SHOULD_QUIT,
    TOK_APP_EXIT,
    TOK_DRAW_PIXEL,
    TOK_DRIVER_SERIAL_WRITE,
    TOK_DRIVER_SERIAL_READ,
    TOK_DRIVER_PORT_READ8,
    TOK_DRIVER_PORT_WRITE8,
    TOK_DRIVER_MMIO_READ32,
    TOK_DRIVER_MMIO_WRITE32,
    TOK_DRIVER_SHOULD_STOP,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_STRUCT,
    TOK_STRING_TYPE,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_FLOAT_NUMBER,
    TOK_STRING,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_SEMI,
    TOK_ASSIGN,
    TOK_COMMA,
    TOK_DOT,
    TOK_PLUS,
    TOK_PLUSPLUS,
    TOK_MINUS,
    TOK_MINUSMINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE
} TokenType;

typedef struct {
    TokenType type;
    char text[PRISMCC_MAX_TOKEN_TEXT];
    int32_t number;
    uint32_t line;
} Token;

typedef struct {
    const char* src;
    uint32_t length;
    uint32_t position;
    uint32_t line;
    Token current;
} Lexer;

typedef struct {
    char name[32];
    uint16_t index;
    uint8_t type;
    uint8_t struct_index;
    uint8_t array_value_type;
    uint8_t array_struct_index;
} Local;

typedef enum {
    LOCAL_TYPE_INT = 0,
    LOCAL_TYPE_STRING = 1,
    LOCAL_TYPE_ARRAY = 2,
    LOCAL_TYPE_STRUCT = 3,
    LOCAL_TYPE_FLOAT = 4,
    LOCAL_TYPE_VOID = 4
} LocalType;

typedef struct {
    char name[32];
    uint8_t type;
} StructField;

typedef struct {
    char name[32];
    uint8_t field_count;
    StructField fields[PRISMCC_MAX_STRUCT_FIELDS];
} StructDef;

typedef struct {
    char name[32];
    uint32_t entry_offset;
    uint8_t param_count;
    uint8_t return_type;
    uint8_t param_types[PRISMCC_MAX_PARAMS];
} FunctionDef;

typedef struct {
    char callee[32];
    uint32_t code_immediate_offset;
    uint8_t arg_count;
    uint8_t expects_value;
    uint8_t arg_types[PRISMCC_MAX_PARAMS];
} CallPatch;

typedef struct {
    Lexer lexer;
    uint8_t code[PRISMCC_MAX_CODE_SIZE];
    uint32_t code_size;
    uint8_t data[PRISMCC_MAX_DATA_SIZE];
    uint32_t data_size;
    Local locals[PRISMCC_MAX_LOCALS];
    uint32_t local_count;
    FunctionDef functions[PRISMCC_MAX_FUNCTIONS];
    uint32_t function_count;
    StructDef structs[PRISMCC_MAX_STRUCTS];
    uint32_t struct_count;
    CallPatch call_patches[PRISMCC_MAX_CALL_PATCHES];
    uint32_t call_patch_count;
    uint32_t main_entry;
    uint8_t current_function_return_type;
    uint8_t driver_mode;
    const char* error;
} PrismCompiler;

static char source_buffer[PRISMCC_MAX_SOURCE_SIZE + 1U];
static char include_file_buffers[PRISMCC_MAX_INCLUDE_DEPTH][PRISMCC_MAX_SOURCE_SIZE + 1U];
static char include_stack[PRISMCC_MAX_INCLUDE_DEPTH][PRISMCC_MAX_PATH];
static uint8_t output_buffer[PRISM_APP_HEADER_SIZE + BCVM_IMAGE_HEADER_SIZE + PRISMCC_MAX_CODE_SIZE + PRISMCC_MAX_DATA_SIZE];
static PrismCompiler compiler;

static void set_error(char* out, uint32_t capacity, const char* message) {
    uint32_t i = 0;

    if (out == 0 || capacity == 0U) {
        return;
    }

    while (message[i] != '\0' && i + 1U < capacity) {
        out[i] = message[i];
        i++;
    }

    out[i] = '\0';
}

static void set_error_with_line(char* out, uint32_t capacity, uint32_t line, const char* message) {
    char line_text[16];
    uint32_t line_len = 0;
    uint32_t i = 0;

    if (out == 0 || capacity == 0U) {
        return;
    }

    if (line == 0U) {
        set_error(out, capacity, message);
        return;
    }

    do {
        uint32_t digit = line % 10U;
        line_text[line_len++] = (char)('0' + digit);
        line /= 10U;
    } while (line > 0U && line_len < sizeof(line_text));

    if (capacity <= 7U) {
        set_error(out, capacity, message);
        return;
    }

    out[i++] = 'l';
    out[i++] = 'i';
    out[i++] = 'n';
    out[i++] = 'e';
    out[i++] = ' ';

    while (line_len > 0U && i + 1U < capacity) {
        out[i++] = line_text[--line_len];
    }

    if (i + 2U < capacity) {
        out[i++] = ':';
        out[i++] = ' ';
    }

    while (message[0] != '\0' && i + 1U < capacity) {
        out[i] = *message;
        i++;
        message++;
    }

    out[i] = '\0';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int string_equals(const char* left, const char* right) {
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }

        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

static uint32_t string_length(const char* text) {
    uint32_t length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

static void copy_string(char* destination, const char* source, uint32_t capacity) {
    uint32_t i = 0;

    if (capacity == 0U) {
        return;
    }

    while (source[i] != '\0' && i + 1U < capacity) {
        destination[i] = source[i];
        i++;
    }

    destination[i] = '\0';
}

static void path_directory(const char* absolute_path, char* out, uint32_t out_capacity) {
    uint32_t length = string_length(absolute_path);
    uint32_t end = length;

    if (out_capacity == 0U) {
        return;
    }

    while (end > 0U && absolute_path[end - 1U] != '/') {
        end--;
    }

    if (end == 0U) {
        copy_string(out, "/", out_capacity);
        return;
    }

    if (end >= out_capacity) {
        end = out_capacity - 1U;
    }

    for (uint32_t i = 0; i < end; i++) {
        out[i] = absolute_path[i];
    }

    if (end == 0U) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    out[end] = '\0';
}

static int append_source_text(const char* text, uint32_t length, uint32_t* io_output_size, char* out_error, uint32_t out_error_capacity) {
    if (*io_output_size + length >= PRISMCC_MAX_SOURCE_SIZE) {
        set_error(out_error, out_error_capacity, "source too large after include expansion");
        return -1;
    }

    for (uint32_t i = 0; i < length; i++) {
        source_buffer[*io_output_size + i] = text[i];
    }

    *io_output_size += length;
    source_buffer[*io_output_size] = '\0';
    return 0;
}

static int append_source_char(char ch, uint32_t* io_output_size, char* out_error, uint32_t out_error_capacity) {
    return append_source_text(&ch, 1U, io_output_size, out_error, out_error_capacity);
}

static int parse_include_directive(const char* line, uint32_t length, char* out_path, uint32_t out_path_capacity) {
    uint32_t pos = 0;
    char terminator;
    uint32_t out_index = 0;

    while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    if (pos >= length || line[pos] != '#') {
        return 0;
    }

    pos++;
    while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    if (pos + 7U > length
        || line[pos + 0U] != 'i'
        || line[pos + 1U] != 'n'
        || line[pos + 2U] != 'c'
        || line[pos + 3U] != 'l'
        || line[pos + 4U] != 'u'
        || line[pos + 5U] != 'd'
        || line[pos + 6U] != 'e') {
        return -1;
    }

    pos += 7U;
    while (pos < length && (line[pos] == ' ' || line[pos] == '\t')) {
        pos++;
    }

    if (pos >= length) {
        return -1;
    }

    if (line[pos] == '"') {
        terminator = '"';
    } else if (line[pos] == '<') {
        terminator = '>';
    } else {
        return -1;
    }

    pos++;
    while (pos < length && line[pos] != terminator) {
        if (out_index + 1U >= out_path_capacity) {
            return -1;
        }

        out_path[out_index++] = line[pos++];
    }

    if (pos >= length || line[pos] != terminator || out_index == 0U) {
        return -1;
    }

    out_path[out_index] = '\0';
    return 1;
}

static int preprocess_file_recursive(const char* absolute_path,
    uint32_t depth,
    uint32_t* io_output_size,
    char* out_error,
    uint32_t out_error_capacity) {
    uint32_t file_size = 0;
    uint32_t line_start = 0;

    if (depth >= PRISMCC_MAX_INCLUDE_DEPTH) {
        set_error(out_error, out_error_capacity, "include nesting too deep");
        return -1;
    }

    for (uint32_t i = 0; i < depth; i++) {
        if (string_equals(include_stack[i], absolute_path)) {
            set_error(out_error, out_error_capacity, "cyclic include detected");
            return -1;
        }
    }

    copy_string(include_stack[depth], absolute_path, sizeof(include_stack[depth]));

    if (vfs_read_file(absolute_path, include_file_buffers[depth], sizeof(include_file_buffers[depth]), &file_size) != 0) {
        set_error(out_error, out_error_capacity, "failed to read include file");
        return -1;
    }

    while (line_start < file_size) {
        uint32_t line_end = line_start;
        int include_result;
        char include_path[PRISMCC_MAX_PATH];

        while (line_end < file_size && include_file_buffers[depth][line_end] != '\n') {
            line_end++;
        }

        include_result = parse_include_directive(&include_file_buffers[depth][line_start],
            line_end - line_start,
            include_path,
            sizeof(include_path));

        if (include_result == 1) {
            char base_dir[PRISMCC_MAX_PATH];
            char resolved_include[PRISMCC_MAX_PATH];

            if (include_path[0] == '/') {
                copy_string(resolved_include, include_path, sizeof(resolved_include));
            } else {
                path_directory(absolute_path, base_dir, sizeof(base_dir));
                if (vfs_normalize_path(base_dir, include_path, resolved_include, sizeof(resolved_include)) != 0) {
                    set_error(out_error, out_error_capacity, "invalid include path");
                    return -1;
                }
            }

            if (preprocess_file_recursive(resolved_include, depth + 1U, io_output_size, out_error, out_error_capacity) != 0) {
                return -1;
            }

            if (append_source_char('\n', io_output_size, out_error, out_error_capacity) != 0) {
                return -1;
            }
        } else if (include_result == 0) {
            if (append_source_text(&include_file_buffers[depth][line_start],
                line_end - line_start,
                io_output_size,
                out_error,
                out_error_capacity) != 0) {
                return -1;
            }

            if (line_end < file_size && include_file_buffers[depth][line_end] == '\n') {
                if (append_source_char('\n', io_output_size, out_error, out_error_capacity) != 0) {
                    return -1;
                }
            }
        } else {
            set_error(out_error, out_error_capacity, "unsupported preprocessor directive");
            return -1;
        }

        if (line_end >= file_size) {
            break;
        }

        line_start = line_end + 1U;
    }

    return 0;
}

static int token_is_name(TokenType type) {
    return type == TOK_IDENTIFIER || type == TOK_MAIN;
}

static int token_is_integer_type(TokenType type) {
    return type == TOK_INT
        || type == TOK_INT8
        || type == TOK_INT16
        || type == TOK_INT32
        || type == TOK_UINT8
        || type == TOK_UINT16
        || type == TOK_UINT32
        || type == TOK_CHAR_TYPE;
}

static int token_is_decl_type(TokenType type) {
    return token_is_integer_type(type)
        || type == TOK_STRING_TYPE
        || type == TOK_FLOAT_TYPE
        || type == TOK_VOID;
}

static LocalType token_to_local_type(TokenType type) {
    if (type == TOK_STRING_TYPE) {
        return LOCAL_TYPE_STRING;
    }

    if (type == TOK_FLOAT_TYPE || type == TOK_FLOAT_NUMBER) {
        return LOCAL_TYPE_FLOAT;
    }

    if (type == TOK_VOID) {
        return LOCAL_TYPE_VOID;
    }

    return LOCAL_TYPE_INT;
}

static int types_compatible(uint8_t target_type, uint8_t source_type) {
    if (target_type == source_type) {
        return 1;
    }

    if (target_type == (uint8_t)LOCAL_TYPE_FLOAT && source_type == (uint8_t)LOCAL_TYPE_INT) {
        return 1;
    }

    return 0;
}

static TokenType keyword_type(const char* text) {
    if (string_equals(text, "int")) {
        return TOK_INT;
    }

    if (string_equals(text, "int8")) {
        return TOK_INT8;
    }

    if (string_equals(text, "int16")) {
        return TOK_INT16;
    }

    if (string_equals(text, "int32")) {
        return TOK_INT32;
    }

    if (string_equals(text, "uint8")) {
        return TOK_UINT8;
    }

    if (string_equals(text, "uint16")) {
        return TOK_UINT16;
    }

    if (string_equals(text, "uint32")) {
        return TOK_UINT32;
    }

    if (string_equals(text, "float")) {
        return TOK_FLOAT_TYPE;
    }

    if (string_equals(text, "void")) {
        return TOK_VOID;
    }

    if (string_equals(text, "char")) {
        return TOK_CHAR_TYPE;
    }

    if (string_equals(text, "main")) {
        return TOK_MAIN;
    }

    if (string_equals(text, "return")) {
        return TOK_RETURN;
    }

    if (string_equals(text, "print")) {
        return TOK_PRINT;
    }

    if (string_equals(text, "print_int")) {
        return TOK_PRINT_INT;
    }

    if (string_equals(text, "print_char")) {
        return TOK_PRINT_CHAR;
    }

    if (string_equals(text, "print_color")) {
        return TOK_PRINT_COLOR;
    }

    if (string_equals(text, "read_text")) {
        return TOK_READ_TEXT;
    }

    if (string_equals(text, "print_input")) {
        return TOK_PRINT_INPUT;
    }

    if (string_equals(text, "input_int")) {
        return TOK_INPUT_INT;
    }

    if (string_equals(text, "input_len")) {
        return TOK_INPUT_LEN;
    }

    if (string_equals(text, "input_eq")) {
        return TOK_INPUT_EQ;
    }

    if (string_equals(text, "string_len")) {
        return TOK_STRING_LEN;
    }

    if (string_equals(text, "string_eq")) {
        return TOK_STRING_EQ;
    }

    if (string_equals(text, "file_read")) {
        return TOK_FILE_READ;
    }

    if (string_equals(text, "file_write")) {
        return TOK_FILE_WRITE;
    }

    if (string_equals(text, "file_append")) {
        return TOK_FILE_APPEND;
    }

    if (string_equals(text, "file_exists")) {
        return TOK_FILE_EXISTS;
    }

    if (string_equals(text, "set_fullscreen")) {
        return TOK_SET_FULLSCREEN;
    }

    if (string_equals(text, "app_should_quit")) {
        return TOK_APP_SHOULD_QUIT;
    }

    if (string_equals(text, "app_exit")) {
        return TOK_APP_EXIT;
    }

    if (string_equals(text, "draw_pixel")) {
        return TOK_DRAW_PIXEL;
    }

    if (string_equals(text, "driver_serial_write")) {
        return TOK_DRIVER_SERIAL_WRITE;
    }

    if (string_equals(text, "driver_serial_read")) {
        return TOK_DRIVER_SERIAL_READ;
    }

    if (string_equals(text, "driver_io_read8")) {
        return TOK_DRIVER_PORT_READ8;
    }

    if (string_equals(text, "driver_io_write8")) {
        return TOK_DRIVER_PORT_WRITE8;
    }

    if (string_equals(text, "driver_mmio_read32")) {
        return TOK_DRIVER_MMIO_READ32;
    }

    if (string_equals(text, "driver_mmio_write32")) {
        return TOK_DRIVER_MMIO_WRITE32;
    }

    if (string_equals(text, "driver_should_stop")) {
        return TOK_DRIVER_SHOULD_STOP;
    }

    if (string_equals(text, "string")) {
        return TOK_STRING_TYPE;
    }

    if (string_equals(text, "if")) {
        return TOK_IF;
    }

    if (string_equals(text, "else")) {
        return TOK_ELSE;
    }

    if (string_equals(text, "while")) {
        return TOK_WHILE;
    }

    if (string_equals(text, "for")) {
        return TOK_FOR;
    }

    if (string_equals(text, "struct")) {
        return TOK_STRUCT;
    }

    return TOK_IDENTIFIER;
}

static void lexer_skip_ws(Lexer* lexer) {
    while (lexer->position < lexer->length) {
        char c = lexer->src[lexer->position];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (c == '\n') {
                lexer->line++;
            }
            lexer->position++;
            continue;
        }

        if (c == '/' && (lexer->position + 1U) < lexer->length && lexer->src[lexer->position + 1U] == '/') {
            lexer->position += 2U;
            while (lexer->position < lexer->length && lexer->src[lexer->position] != '\n') {
                lexer->position++;
            }
            continue;
        }

        break;
    }
}

static Token lexer_next(Lexer* lexer, const char** out_error) {
    Token token;
    uint32_t text_index = 0;

    token.type = TOK_EOF;
    token.text[0] = '\0';
    token.number = 0;
    token.line = lexer->line;

    lexer_skip_ws(lexer);

    if (lexer->position >= lexer->length) {
        token.type = TOK_EOF;
        return token;
    }

    {
        char c = lexer->src[lexer->position++];

        if (is_alpha(c)) {
            token.text[text_index++] = c;
            while (lexer->position < lexer->length
                && (is_alpha(lexer->src[lexer->position]) || is_digit(lexer->src[lexer->position]))) {
                if (text_index + 1U >= PRISMCC_MAX_TOKEN_TEXT) {
                    *out_error = "identifier too long";
                    return token;
                }

                token.text[text_index++] = lexer->src[lexer->position++];
            }

            token.text[text_index] = '\0';
            token.type = keyword_type(token.text);
            return token;
        }

        if (is_digit(c)) {
            int32_t value = (int32_t)(c - '0');
            int is_float = 0;
            int32_t frac = 0;
            int frac_digits = 0;
            while (lexer->position < lexer->length && is_digit(lexer->src[lexer->position])) {
                value = value * 10 + (int32_t)(lexer->src[lexer->position] - '0');
                lexer->position++;
            }

            if (lexer->position + 1U < lexer->length
                && lexer->src[lexer->position] == '.'
                && is_digit(lexer->src[lexer->position + 1U])) {
                is_float = 1;
                lexer->position++;

                while (lexer->position < lexer->length && is_digit(lexer->src[lexer->position])) {
                    if (frac_digits < 3) {
                        frac = frac * 10 + (int32_t)(lexer->src[lexer->position] - '0');
                        frac_digits++;
                    }
                    lexer->position++;
                }

                while (frac_digits < 3) {
                    frac *= 10;
                    frac_digits++;
                }
            }

            token.type = is_float ? TOK_FLOAT_NUMBER : TOK_NUMBER;
            token.number = is_float ? (value * PRISMCC_FLOAT_SCALE + frac) : value;
            return token;
        }

        switch (c) {
            case '(': token.type = TOK_LPAREN; return token;
            case ')': token.type = TOK_RPAREN; return token;
            case '[': token.type = TOK_LBRACKET; return token;
            case ']': token.type = TOK_RBRACKET; return token;
            case '{': token.type = TOK_LBRACE; return token;
            case '}': token.type = TOK_RBRACE; return token;
            case ';': token.type = TOK_SEMI; return token;
            case ',': token.type = TOK_COMMA; return token;
            case '.': token.type = TOK_DOT; return token;
            case '+':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '+') {
                    lexer->position++;
                    token.type = TOK_PLUSPLUS;
                } else {
                    token.type = TOK_PLUS;
                }
                return token;
            case '-':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '-') {
                    lexer->position++;
                    token.type = TOK_MINUSMINUS;
                } else {
                    token.type = TOK_MINUS;
                }
                return token;
            case '*': token.type = TOK_STAR; return token;
            case '/': token.type = TOK_SLASH; return token;
            case '%': token.type = TOK_PERCENT; return token;
            case '=':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '=') {
                    lexer->position++;
                    token.type = TOK_EQEQ;
                } else {
                    token.type = TOK_ASSIGN;
                }
                return token;
            case '!':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '=') {
                    lexer->position++;
                    token.type = TOK_NEQ;
                    return token;
                }

                *out_error = "unexpected character";
                return token;
            case '<':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '=') {
                    lexer->position++;
                    token.type = TOK_LTE;
                } else {
                    token.type = TOK_LT;
                }
                return token;
            case '>':
                if (lexer->position < lexer->length && lexer->src[lexer->position] == '=') {
                    lexer->position++;
                    token.type = TOK_GTE;
                } else {
                    token.type = TOK_GT;
                }
                return token;
            case '"':
                while (lexer->position < lexer->length) {
                    char sc = lexer->src[lexer->position++];

                    if (sc == '"') {
                        token.text[text_index] = '\0';
                        token.type = TOK_STRING;
                        return token;
                    }

                    if (sc == '\\') {
                        if (lexer->position >= lexer->length) {
                            *out_error = "unterminated string";
                            return token;
                        }

                        sc = lexer->src[lexer->position++];
                        if (sc == 'n') {
                            sc = '\n';
                        } else if (sc == 't') {
                            sc = '\t';
                        }
                    }

                    if (text_index + 1U >= PRISMCC_MAX_TOKEN_TEXT) {
                        *out_error = "string too long";
                        return token;
                    }

                    token.text[text_index++] = sc;
                }

                *out_error = "unterminated string";
                return token;
            case '\'': {
                char sc;

                if (lexer->position >= lexer->length) {
                    *out_error = "unterminated char literal";
                    return token;
                }

                if (lexer->src[lexer->position] == '\'') {
                    *out_error = "char literal must contain one character";
                    return token;
                }

                sc = lexer->src[lexer->position++];
                if (sc == '\\') {
                    if (lexer->position >= lexer->length) {
                        *out_error = "unterminated char literal escape";
                        return token;
                    }

                    sc = lexer->src[lexer->position++];
                    if (sc == 'n') {
                        sc = '\n';
                    } else if (sc == 't') {
                        sc = '\t';
                    } else if (sc == '\\') {
                        sc = '\\';
                    } else if (sc == '\'') {
                        sc = '\'';
                    }
                }

                if (lexer->position >= lexer->length || lexer->src[lexer->position] != '\'') {
                    *out_error = "char literal must contain one character";
                    return token;
                }

                lexer->position++;
                token.type = TOK_NUMBER;
                token.number = (int32_t)(uint8_t)sc;
                return token;
            }
            default:
                *out_error = "unexpected character";
                return token;
        }
    }
}

static void next_token(PrismCompiler* compiler) {
    const char* lexer_error = 0;

    compiler->lexer.current = lexer_next(&compiler->lexer, &lexer_error);
    if (lexer_error != 0 && compiler->error == 0) {
        compiler->error = lexer_error;
    }
}

static int expect(PrismCompiler* compiler, TokenType type, const char* message) {
    if (compiler->error != 0) {
        return -1;
    }

    if (compiler->lexer.current.type != type) {
        compiler->error = message;
        return -1;
    }

    next_token(compiler);
    return 0;
}

static int emit_u8(PrismCompiler* compiler, uint8_t value) {
    if (compiler->code_size >= PRISMCC_MAX_CODE_SIZE) {
        compiler->error = "code buffer full";
        return -1;
    }

    compiler->code[compiler->code_size++] = value;
    return 0;
}

static int emit_u16(PrismCompiler* compiler, uint16_t value) {
    if (emit_u8(compiler, (uint8_t)(value & 0xFFU)) != 0) {
        return -1;
    }

    return emit_u8(compiler, (uint8_t)((value >> 8) & 0xFFU));
}

static int emit_u32(PrismCompiler* compiler, uint32_t value) {
    if (emit_u8(compiler, (uint8_t)(value & 0xFFU)) != 0) {
        return -1;
    }
    if (emit_u8(compiler, (uint8_t)((value >> 8) & 0xFFU)) != 0) {
        return -1;
    }
    if (emit_u8(compiler, (uint8_t)((value >> 16) & 0xFFU)) != 0) {
        return -1;
    }
    return emit_u8(compiler, (uint8_t)((value >> 24) & 0xFFU));
}

static void patch_u32_at(PrismCompiler* compiler, uint32_t offset, uint32_t value) {
    if (offset + 4U > compiler->code_size) {
        compiler->error = "internal patch range error";
        return;
    }

    compiler->code[offset] = (uint8_t)(value & 0xFFU);
    compiler->code[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
    compiler->code[offset + 2U] = (uint8_t)((value >> 16) & 0xFFU);
    compiler->code[offset + 3U] = (uint8_t)((value >> 24) & 0xFFU);
}

static int emit_jump_placeholder(PrismCompiler* compiler, uint8_t opcode, uint32_t* out_immediate_offset) {
    if (emit_u8(compiler, opcode) != 0) {
        return -1;
    }

    *out_immediate_offset = compiler->code_size;
    return emit_u32(compiler, 0U);
}

static int add_data(PrismCompiler* compiler, const char* bytes, uint16_t length, uint16_t* out_offset) {
    uint32_t end = compiler->data_size + (uint32_t)length;

    if (end > PRISMCC_MAX_DATA_SIZE) {
        compiler->error = "data section full";
        return -1;
    }

    *out_offset = (uint16_t)compiler->data_size;
    for (uint32_t i = 0; i < (uint32_t)length; i++) {
        compiler->data[compiler->data_size + i] = (uint8_t)bytes[i];
    }

    compiler->data_size = end;
    return 0;
}

static Local* find_local_entry(PrismCompiler* compiler, const char* name) {
    for (uint32_t i = 0; i < compiler->local_count; i++) {
        if (string_equals(compiler->locals[i].name, name)) {
            return &compiler->locals[i];
        }
    }

    return 0;
}

static StructDef* find_struct_def(PrismCompiler* compiler, const char* name) {
    for (uint32_t i = 0; i < compiler->struct_count; i++) {
        if (string_equals(compiler->structs[i].name, name)) {
            return &compiler->structs[i];
        }
    }

    return 0;
}

static int find_struct_field_index(const StructDef* def, const char* field_name) {
    for (uint32_t i = 0; i < def->field_count; i++) {
        if (string_equals(def->fields[i].name, field_name)) {
            return (int)i;
        }
    }

    return -1;
}

static const StructField* find_struct_field(const StructDef* def, const char* field_name) {
    int index = find_struct_field_index(def, field_name);
    if (index < 0) {
        return 0;
    }

    return &def->fields[(uint32_t)index];
}

static int add_struct_def(PrismCompiler* compiler, const char* name, StructDef** out_struct) {
    if (compiler->struct_count >= PRISMCC_MAX_STRUCTS) {
        compiler->error = "too many struct definitions";
        return -1;
    }

    if (find_struct_def(compiler, name) != 0) {
        compiler->error = "duplicate struct definition";
        return -1;
    }

    copy_string(compiler->structs[compiler->struct_count].name,
        name,
        sizeof(compiler->structs[compiler->struct_count].name));
    compiler->structs[compiler->struct_count].field_count = 0;
    *out_struct = &compiler->structs[compiler->struct_count];
    compiler->struct_count++;
    return 0;
}

static int find_local(PrismCompiler* compiler, const char* name) {
    Local* local = find_local_entry(compiler, name);
    return local != 0 ? (int)local->index : -1;
}

static int add_local(PrismCompiler* compiler, const char* name, LocalType type, uint16_t* out_index) {
    if (compiler->local_count >= PRISMCC_MAX_LOCALS) {
        compiler->error = "too many locals";
        return -1;
    }

    if (find_local(compiler, name) >= 0) {
        compiler->error = "duplicate local";
        return -1;
    }

    copy_string(compiler->locals[compiler->local_count].name, name, sizeof(compiler->locals[compiler->local_count].name));
    compiler->locals[compiler->local_count].index = (uint16_t)compiler->local_count;
    compiler->locals[compiler->local_count].type = (uint8_t)type;
    compiler->locals[compiler->local_count].struct_index = 0xFFU;
    compiler->locals[compiler->local_count].array_value_type = (uint8_t)LOCAL_TYPE_INT;
    compiler->locals[compiler->local_count].array_struct_index = 0xFFU;
    *out_index = (uint16_t)compiler->local_count;
    compiler->local_count++;
    return 0;
}

static uint32_t make_literal_string_descriptor(uint16_t offset, uint16_t length) {
    return ((uint32_t)offset << 16) | (uint32_t)length;
}

static int signature_types_equal(const uint8_t* left, const uint8_t* right, uint8_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (left[i] != right[i]) {
            return 0;
        }
    }

    return 1;
}

static int find_function_overload(PrismCompiler* compiler, const char* name, uint8_t arg_count, const uint8_t* arg_types) {
    for (uint32_t i = 0; i < compiler->function_count; i++) {
        if (!string_equals(compiler->functions[i].name, name)) {
            continue;
        }

        if (compiler->functions[i].param_count != arg_count) {
            continue;
        }

        if (signature_types_equal(compiler->functions[i].param_types, arg_types, arg_count)) {
            return (int)i;
        }
    }

    return -1;
}

static int add_function(PrismCompiler* compiler,
    const char* name,
    uint32_t entry_offset,
    uint8_t param_count,
    LocalType return_type,
    const uint8_t* param_types) {
    if (compiler->function_count >= PRISMCC_MAX_FUNCTIONS) {
        compiler->error = "too many functions";
        return -1;
    }

    if (find_function_overload(compiler, name, param_count, param_types) >= 0) {
        compiler->error = "duplicate function signature";
        return -1;
    }

    copy_string(compiler->functions[compiler->function_count].name, name, sizeof(compiler->functions[compiler->function_count].name));
    compiler->functions[compiler->function_count].entry_offset = entry_offset;
    compiler->functions[compiler->function_count].param_count = param_count;
    compiler->functions[compiler->function_count].return_type = (uint8_t)return_type;
    for (uint32_t i = 0; i < param_count; i++) {
        compiler->functions[compiler->function_count].param_types[i] = param_types[i];
    }
    compiler->function_count++;
    return 0;
}

static int add_call_patch(PrismCompiler* compiler,
    const char* callee,
    uint32_t immediate_offset,
    uint8_t arg_count,
    const uint8_t* arg_types,
    int expects_value) {
    if (compiler->call_patch_count >= PRISMCC_MAX_CALL_PATCHES) {
        compiler->error = "too many call sites";
        return -1;
    }

    copy_string(compiler->call_patches[compiler->call_patch_count].callee,
        callee,
        sizeof(compiler->call_patches[compiler->call_patch_count].callee));
    compiler->call_patches[compiler->call_patch_count].code_immediate_offset = immediate_offset;
    compiler->call_patches[compiler->call_patch_count].arg_count = arg_count;
    compiler->call_patches[compiler->call_patch_count].expects_value = expects_value ? 1U : 0U;
    for (uint32_t i = 0; i < arg_count; i++) {
        compiler->call_patches[compiler->call_patch_count].arg_types[i] = arg_types[i];
    }
    compiler->call_patch_count++;
    return 0;
}

static int emit_call_by_name(PrismCompiler* compiler, const char* name, uint8_t arg_count, const uint8_t* arg_types) {
    uint32_t patch_offset;

    if (emit_u8(compiler, BCVM_OP_CALL_ARGS) != 0) {
        return -1;
    }

    patch_offset = compiler->code_size;
    if (emit_u32(compiler, 0U) != 0) {
        return -1;
    }

    if (emit_u8(compiler, arg_count) != 0) {
        return -1;
    }

    return add_call_patch(compiler, name, patch_offset, arg_count, arg_types, 0);
}

static int parse_expression(PrismCompiler* compiler);
static int parse_statement(PrismCompiler* compiler, int* saw_return);

static uint8_t guess_expression_type(PrismCompiler* compiler) {
    TokenType type = compiler->lexer.current.type;

    if (type == TOK_PLUS || type == TOK_MINUS) {
        Lexer peek = compiler->lexer;
        const char* lexer_error = 0;
        Token next = lexer_next(&peek, &lexer_error);

        if (lexer_error == 0 && next.type == TOK_FLOAT_NUMBER) {
            return (uint8_t)LOCAL_TYPE_FLOAT;
        }
    }

    if (type == TOK_FLOAT_NUMBER) {
        return (uint8_t)LOCAL_TYPE_FLOAT;
    }

    if (type == TOK_STRING || type == TOK_READ_TEXT || type == TOK_FILE_READ) {
        return (uint8_t)LOCAL_TYPE_STRING;
    }

    if (token_is_name(type)) {
        Local* local = find_local_entry(compiler, compiler->lexer.current.text);
        if (local != 0) {
            Lexer peek = compiler->lexer;
            const char* lexer_error = 0;
            Token after_name = lexer_next(&peek, &lexer_error);

            if (lexer_error == 0
                && local->type == (uint8_t)LOCAL_TYPE_ARRAY
                && after_name.type == TOK_LBRACKET) {
                return local->array_value_type;
            }

            if (local->type == (uint8_t)LOCAL_TYPE_STRUCT) {
                if (lexer_error == 0 && after_name.type == TOK_DOT) {
                    Token field = lexer_next(&peek, &lexer_error);

                    if (lexer_error == 0 && token_is_name(field.type) && local->struct_index != 0xFFU) {
                        StructDef* def = &compiler->structs[local->struct_index];
                        const StructField* struct_field = find_struct_field(def, field.text);
                        if (struct_field != 0) {
                            return struct_field->type;
                        }
                    }
                }
            }

            return local->type;
        }
    }

    return (uint8_t)LOCAL_TYPE_INT;
}

static int parse_declaration(PrismCompiler* compiler,
    LocalType type,
    TokenType type_token,
    const char* type_name,
    int expect_semicolon,
    int allow_reuse_existing) {
    char name[PRISMCC_MAX_TOKEN_TEXT];
    uint16_t local_index;
    LocalType effective_type = type;
    uint8_t array_value_type = (uint8_t)type;
    uint16_t array_length = 0;
    Local* existing_local = 0;

    if (expect(compiler, type_token, "expected declaration type") != 0) {
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = type_name;
        return -1;
    }

    copy_string(name, compiler->lexer.current.text, sizeof(name));
    next_token(compiler);

    if (compiler->lexer.current.type == TOK_LBRACKET) {
        next_token(compiler);

        if (compiler->lexer.current.type != TOK_NUMBER || compiler->lexer.current.number <= 0) {
            compiler->error = "array size must be a positive constant";
            return -1;
        }

        array_length = (uint16_t)compiler->lexer.current.number;
        next_token(compiler);

        if (expect(compiler, TOK_RBRACKET, "expected ']' after array size") != 0) {
            return -1;
        }

        effective_type = LOCAL_TYPE_ARRAY;
    }

    if (allow_reuse_existing) {
        existing_local = find_local_entry(compiler, name);
    }

    if (existing_local != 0) {
        if (existing_local->type != (uint8_t)effective_type
            || (effective_type == LOCAL_TYPE_ARRAY && existing_local->array_value_type != array_value_type)) {
            compiler->error = "conflicting local type for reused for variable";
            return -1;
        }

        local_index = existing_local->index;
    } else {
        if (add_local(compiler, name, effective_type, &local_index) != 0) {
            return -1;
        }

        if (effective_type == LOCAL_TYPE_ARRAY) {
            compiler->locals[local_index].array_value_type = array_value_type;
            compiler->locals[local_index].array_struct_index = 0xFFU;
        }
    }

    if (effective_type == LOCAL_TYPE_ARRAY) {
        if (compiler->lexer.current.type == TOK_ASSIGN) {
            compiler->error = "array initializer is not supported yet";
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_ARR_NEW) != 0 || emit_u16(compiler, array_length) != 0) {
            return -1;
        }
    } else if (compiler->lexer.current.type == TOK_ASSIGN) {
        uint8_t rhs_type;

        next_token(compiler);
        rhs_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (!types_compatible((uint8_t)effective_type, rhs_type)) {
            compiler->error = "initializer type mismatch";
            return -1;
        }

        if ((uint8_t)effective_type == (uint8_t)LOCAL_TYPE_FLOAT && rhs_type == (uint8_t)LOCAL_TYPE_INT) {
            if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0
                || emit_u32(compiler, PRISMCC_FLOAT_SCALE) != 0
                || emit_u8(compiler, BCVM_OP_MUL) != 0) {
                return -1;
            }
        }
    } else {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, 0U) != 0) {
            return -1;
        }
    }

    if (emit_u8(compiler, BCVM_OP_STORE_LOCAL) != 0 || emit_u16(compiler, local_index) != 0) {
        return -1;
    }

    if (expect_semicolon) {
        return expect(compiler, TOK_SEMI, "expected ';' after declaration");
    }

    return 0;
}

static int parse_int_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT, "expected identifier after int", expect_semicolon, 0);
}

static int parse_char_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_CHAR_TYPE, "expected identifier after char", expect_semicolon, 0);
}

static int parse_string_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_STRING, TOK_STRING_TYPE, "expected identifier after string", expect_semicolon, 0);
}

static int parse_float_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_FLOAT, TOK_FLOAT_TYPE, "expected identifier after float", expect_semicolon, 0);
}

static int parse_int8_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT8, "expected identifier after int8", expect_semicolon, 0);
}

static int parse_int16_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT16, "expected identifier after int16", expect_semicolon, 0);
}

static int parse_int32_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT32, "expected identifier after int32", expect_semicolon, 0);
}

static int parse_uint8_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT8, "expected identifier after uint8", expect_semicolon, 0);
}

static int parse_uint16_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT16, "expected identifier after uint16", expect_semicolon, 0);
}

static int parse_uint32_declaration(PrismCompiler* compiler, int expect_semicolon) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT32, "expected identifier after uint32", expect_semicolon, 0);
}

static int parse_for_int_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT, "expected identifier after int", 0, 1);
}

static int parse_for_char_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_CHAR_TYPE, "expected identifier after char", 0, 1);
}

static int parse_for_string_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_STRING, TOK_STRING_TYPE, "expected identifier after string", 0, 1);
}

static int parse_for_float_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_FLOAT, TOK_FLOAT_TYPE, "expected identifier after float", 0, 1);
}

static int parse_for_int8_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT8, "expected identifier after int8", 0, 1);
}

static int parse_for_int16_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT16, "expected identifier after int16", 0, 1);
}

static int parse_for_int32_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_INT32, "expected identifier after int32", 0, 1);
}

static int parse_for_uint8_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT8, "expected identifier after uint8", 0, 1);
}

static int parse_for_uint16_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT16, "expected identifier after uint16", 0, 1);
}

static int parse_for_uint32_declaration(PrismCompiler* compiler) {
    return parse_declaration(compiler, LOCAL_TYPE_INT, TOK_UINT32, "expected identifier after uint32", 0, 1);
}

static int parse_struct_variable_declaration(PrismCompiler* compiler, int expect_semicolon) {
    char struct_name[PRISMCC_MAX_TOKEN_TEXT];
    char variable_name[PRISMCC_MAX_TOKEN_TEXT];
    StructDef* def;
    uint16_t local_index;
    uint16_t array_length = 0;
    int is_array = 0;

    if (expect(compiler, TOK_STRUCT, "expected 'struct'") != 0) {
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected struct type name";
        return -1;
    }

    copy_string(struct_name, compiler->lexer.current.text, sizeof(struct_name));
    next_token(compiler);

    def = find_struct_def(compiler, struct_name);
    if (def == 0) {
        compiler->error = "unknown struct type";
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected struct variable name";
        return -1;
    }

    copy_string(variable_name, compiler->lexer.current.text, sizeof(variable_name));
    next_token(compiler);

    if (compiler->lexer.current.type == TOK_LBRACKET) {
        next_token(compiler);

        if (compiler->lexer.current.type != TOK_NUMBER || compiler->lexer.current.number <= 0) {
            compiler->error = "array size must be a positive constant";
            return -1;
        }

        array_length = (uint16_t)compiler->lexer.current.number;
        next_token(compiler);

        if (expect(compiler, TOK_RBRACKET, "expected ']' after array size") != 0) {
            return -1;
        }

        is_array = 1;
    }

    if (add_local(compiler, variable_name, is_array ? LOCAL_TYPE_ARRAY : LOCAL_TYPE_STRUCT, &local_index) != 0) {
        return -1;
    }

    compiler->locals[local_index].struct_index = (uint8_t)(def - compiler->structs);
    compiler->locals[local_index].array_value_type = is_array ? (uint8_t)LOCAL_TYPE_STRUCT : (uint8_t)LOCAL_TYPE_INT;
    compiler->locals[local_index].array_struct_index = is_array ? (uint8_t)(def - compiler->structs) : 0xFFU;

    if (compiler->lexer.current.type == TOK_ASSIGN) {
        compiler->error = "struct initializer is not supported yet";
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_ARR_NEW) != 0 || emit_u16(compiler, is_array ? array_length : def->field_count) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_STORE_LOCAL) != 0 || emit_u16(compiler, local_index) != 0) {
        return -1;
    }

    if (expect_semicolon) {
        return expect(compiler, TOK_SEMI, "expected ';' after struct declaration");
    }

    return 0;
}

static int parse_struct_definition(PrismCompiler* compiler) {
    char struct_name[PRISMCC_MAX_TOKEN_TEXT];
    StructDef* def;

    if (expect(compiler, TOK_STRUCT, "expected 'struct'") != 0) {
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected struct name";
        return -1;
    }

    copy_string(struct_name, compiler->lexer.current.text, sizeof(struct_name));
    next_token(compiler);

    if (add_struct_def(compiler, struct_name, &def) != 0) {
        return -1;
    }

    if (expect(compiler, TOK_LBRACE, "expected '{' in struct definition") != 0) {
        return -1;
    }

    while (compiler->lexer.current.type != TOK_RBRACE && compiler->lexer.current.type != TOK_EOF) {
        TokenType field_type_token = compiler->lexer.current.type;
        LocalType field_type;

        if (!token_is_integer_type(field_type_token)
            && field_type_token != TOK_STRING_TYPE
            && field_type_token != TOK_FLOAT_TYPE) {
            compiler->error = "struct fields must be integer, char, string or float";
            return -1;
        }

        field_type = token_to_local_type(field_type_token);

        next_token(compiler);

        if (!token_is_name(compiler->lexer.current.type)) {
            compiler->error = "expected struct field name";
            return -1;
        }

        if (find_struct_field(def, compiler->lexer.current.text) != 0) {
            compiler->error = "duplicate struct field";
            return -1;
        }

        if (def->field_count >= PRISMCC_MAX_STRUCT_FIELDS) {
            compiler->error = "too many struct fields";
            return -1;
        }

        copy_string(def->fields[def->field_count].name,
            compiler->lexer.current.text,
            sizeof(def->fields[def->field_count].name));
        def->fields[def->field_count].type = (uint8_t)field_type;
        def->field_count++;

        next_token(compiler);

        if (expect(compiler, TOK_SEMI, "expected ';' after struct field") != 0) {
            return -1;
        }
    }

    if (expect(compiler, TOK_RBRACE, "expected '}' after struct fields") != 0) {
        return -1;
    }

    return expect(compiler, TOK_SEMI, "expected ';' after struct definition");
}

static int parse_call_after_name(PrismCompiler* compiler, const char* name, int discard_return) {
    uint8_t arg_count = 0;
    uint8_t arg_types[PRISMCC_MAX_PARAMS];

    if (expect(compiler, TOK_LPAREN, "expected '(' in function call") != 0) {
        return -1;
    }

    if (compiler->lexer.current.type != TOK_RPAREN) {
        while (1) {
            if (arg_count >= PRISMCC_MAX_PARAMS) {
                compiler->error = "too many call arguments";
                return -1;
            }

            arg_types[arg_count] = guess_expression_type(compiler);

            if (parse_expression(compiler) != 0) {
                return -1;
            }

            arg_count++;
            if (compiler->lexer.current.type != TOK_COMMA) {
                break;
            }

            next_token(compiler);
        }
    }

    if (expect(compiler, TOK_RPAREN, "expected ')' after call arguments") != 0) {
        return -1;
    }

    if (emit_call_by_name(compiler, name, arg_count, arg_types) != 0) {
        return -1;
    }

    compiler->call_patches[compiler->call_patch_count - 1U].expects_value = discard_return ? 0U : 1U;

    if (discard_return) {
        return emit_u8(compiler, BCVM_OP_POP);
    }

    return 0;
}

static int parse_assignment_after_name(PrismCompiler* compiler, const char* name) {
    Local* local = find_local_entry(compiler, name);
    uint16_t local_index;
    uint8_t rhs_type;

    if (local == 0) {
        compiler->error = "assignment to unknown identifier";
        return -1;
    }

    if (local->type == (uint8_t)LOCAL_TYPE_ARRAY) {
        compiler->error = "array assignment requires index";
        return -1;
    }

    local_index = local->index;

    if (expect(compiler, TOK_ASSIGN, "expected '=' in assignment") != 0) {
        return -1;
    }

    rhs_type = guess_expression_type(compiler);
    if (parse_expression(compiler) != 0) {
        return -1;
    }

    if (!types_compatible(local->type, rhs_type)) {
        compiler->error = "assignment type mismatch";
        return -1;
    }

    if (local->type == (uint8_t)LOCAL_TYPE_FLOAT && rhs_type == (uint8_t)LOCAL_TYPE_INT) {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0
            || emit_u32(compiler, PRISMCC_FLOAT_SCALE) != 0
            || emit_u8(compiler, BCVM_OP_MUL) != 0) {
            return -1;
        }
    }

    if (emit_u8(compiler, BCVM_OP_STORE_LOCAL) != 0 || emit_u16(compiler, local_index) != 0) {
        return -1;
    }

    return 0;
}

static int emit_local_update(PrismCompiler* compiler, Local* local, int delta, int keep_result, int postfix_result) {
    uint8_t op;

    if (local == 0) {
        compiler->error = "assignment to unknown identifier";
        return -1;
    }

    if (local->type != (uint8_t)LOCAL_TYPE_INT) {
        compiler->error = "increment/decrement requires int variable";
        return -1;
    }

    op = (delta >= 0) ? BCVM_OP_ADD : BCVM_OP_SUB;

    if (postfix_result) {
        if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
            return -1;
        }
    }

    if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, 1U) != 0) {
        return -1;
    }

    if (emit_u8(compiler, op) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_STORE_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
        return -1;
    }

    if (keep_result && !postfix_result) {
        if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_prefix_update(PrismCompiler* compiler, int delta) {
    char name[PRISMCC_MAX_TOKEN_TEXT];
    Local* local;

    next_token(compiler);

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected identifier after increment/decrement";
        return -1;
    }

    copy_string(name, compiler->lexer.current.text, sizeof(name));
    next_token(compiler);

    local = find_local_entry(compiler, name);
    return emit_local_update(compiler, local, delta, 1, 0);
}

static int parse_array_store_after_name(PrismCompiler* compiler, const char* name) {
    Local* local = find_local_entry(compiler, name);
    uint8_t rhs_type;

    if (local == 0) {
        compiler->error = "assignment to unknown identifier";
        return -1;
    }

    if (local->type != (uint8_t)LOCAL_TYPE_ARRAY) {
        compiler->error = "indexed assignment requires an array variable";
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
        return -1;
    }

    if (expect(compiler, TOK_LBRACKET, "expected '[' in array assignment") != 0) {
        return -1;
    }

    if (parse_expression(compiler) != 0) {
        return -1;
    }

    if (expect(compiler, TOK_RBRACKET, "expected ']' after array index") != 0) {
        return -1;
    }

    if (expect(compiler, TOK_ASSIGN, "expected '=' in assignment") != 0) {
        return -1;
    }

    rhs_type = guess_expression_type(compiler);
    if (parse_expression(compiler) != 0) {
        return -1;
    }

    if (!types_compatible(local->array_value_type, rhs_type)) {
        compiler->error = "array element type mismatch";
        return -1;
    }

    if (local->array_value_type == (uint8_t)LOCAL_TYPE_FLOAT && rhs_type == (uint8_t)LOCAL_TYPE_INT) {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0
            || emit_u32(compiler, PRISMCC_FLOAT_SCALE) != 0
            || emit_u8(compiler, BCVM_OP_MUL) != 0) {
            return -1;
        }
    }

    return emit_u8(compiler, BCVM_OP_ARR_SET);
}

static int parse_struct_store_after_name(PrismCompiler* compiler, const char* name) {
    Local* local = find_local_entry(compiler, name);
    StructDef* def;
    int field_index;

    if (local == 0) {
        compiler->error = "assignment to unknown identifier";
        return -1;
    }

    if (local->type != (uint8_t)LOCAL_TYPE_STRUCT || local->struct_index == 0xFFU) {
        compiler->error = "field assignment requires a struct variable";
        return -1;
    }

    def = &compiler->structs[local->struct_index];

    if (expect(compiler, TOK_DOT, "expected '.' in field assignment") != 0) {
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected field name";
        return -1;
    }

    field_index = find_struct_field_index(def, compiler->lexer.current.text);
    if (field_index < 0) {
        compiler->error = "unknown struct field";
        return -1;
    }

    next_token(compiler);

    if (expect(compiler, TOK_ASSIGN, "expected '=' in field assignment") != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u8(compiler, local->index) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, (uint32_t)field_index) != 0) {
        return -1;
    }

    if (parse_expression(compiler) != 0) {
        return -1;
    }

    return emit_u8(compiler, BCVM_OP_ARR_SET);
}

static int parse_for_clause_item(PrismCompiler* compiler, int allow_declaration) {
    if (compiler->lexer.current.type == TOK_INT) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_int_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_CHAR_TYPE) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_char_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_STRING_TYPE) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_string_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_STRUCT) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }

        return parse_struct_variable_declaration(compiler, 0);
    }

    if (compiler->lexer.current.type == TOK_FLOAT_TYPE) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_float_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_INT8) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_int8_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_INT16) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_int16_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_INT32) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_int32_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_UINT8) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_uint8_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_UINT16) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_uint16_declaration(compiler);
    }

    if (compiler->lexer.current.type == TOK_UINT32) {
        if (!allow_declaration) {
            compiler->error = "for increment does not support declaration";
            return -1;
        }
        return parse_for_uint32_declaration(compiler);
    }

    if (token_is_name(compiler->lexer.current.type)) {
        char name[PRISMCC_MAX_TOKEN_TEXT];
        Local* local;

        copy_string(name, compiler->lexer.current.text, sizeof(name));
        next_token(compiler);

        local = find_local_entry(compiler, name);

        if (compiler->lexer.current.type == TOK_ASSIGN) {
            return parse_assignment_after_name(compiler, name);
        }

        if (compiler->lexer.current.type == TOK_DOT) {
            return parse_struct_store_after_name(compiler, name);
        }

        if (compiler->lexer.current.type == TOK_LBRACKET) {
            return parse_array_store_after_name(compiler, name);
        }

        if (compiler->lexer.current.type == TOK_LPAREN) {
            return parse_call_after_name(compiler, name, 1);
        }

        if (compiler->lexer.current.type == TOK_PLUSPLUS) {
            next_token(compiler);
            return emit_local_update(compiler, local, 1, 0, 0);
        }

        if (compiler->lexer.current.type == TOK_MINUSMINUS) {
            next_token(compiler);
            return emit_local_update(compiler, local, -1, 0, 0);
        }

        compiler->error = "unsupported for clause item";
        return -1;
    }

    if (parse_expression(compiler) != 0) {
        return -1;
    }

    return emit_u8(compiler, BCVM_OP_POP);
}

static int parse_driver_builtin(PrismCompiler* compiler, TokenType builtin) {
    uint8_t first_type;
    uint8_t second_type;

    if (!compiler->driver_mode) {
        compiler->error = "driver hardware APIs are only available in .pdr/.pdrv modules";
        return -1;
    }

    next_token(compiler);
    if (expect(compiler, TOK_LPAREN, "expected '(' after driver API") != 0) {
        return -1;
    }

    if (builtin == TOK_DRIVER_SERIAL_READ || builtin == TOK_DRIVER_SHOULD_STOP) {
        if (expect(compiler, TOK_RPAREN, "expected ')' after driver API") != 0) {
            return -1;
        }
    } else {
        first_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (builtin == TOK_DRIVER_SERIAL_WRITE) {
            if (first_type != (uint8_t)LOCAL_TYPE_STRING) {
                compiler->error = "driver_serial_write requires a string";
                return -1;
            }
        } else if (first_type != (uint8_t)LOCAL_TYPE_INT) {
            compiler->error = "driver hardware addresses and values must be integers";
            return -1;
        }

        if (builtin == TOK_DRIVER_PORT_WRITE8 || builtin == TOK_DRIVER_MMIO_WRITE32) {
            if (expect(compiler, TOK_COMMA, "expected ',' before driver value") != 0) {
                return -1;
            }

            second_type = guess_expression_type(compiler);
            if (parse_expression(compiler) != 0) {
                return -1;
            }
            if (second_type != (uint8_t)LOCAL_TYPE_INT) {
                compiler->error = "driver hardware addresses and values must be integers";
                return -1;
            }
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after driver API arguments") != 0) {
            return -1;
        }
    }

    switch (builtin) {
        case TOK_DRIVER_SERIAL_WRITE:
            return emit_u8(compiler, BCVM_OP_DRIVER_SERIAL_WRITE);
        case TOK_DRIVER_SERIAL_READ:
            return emit_u8(compiler, BCVM_OP_DRIVER_SERIAL_READ);
        case TOK_DRIVER_PORT_READ8:
            return emit_u8(compiler, BCVM_OP_DRIVER_PORT_READ8);
        case TOK_DRIVER_PORT_WRITE8:
            return emit_u8(compiler, BCVM_OP_DRIVER_PORT_WRITE8);
        case TOK_DRIVER_MMIO_READ32:
            return emit_u8(compiler, BCVM_OP_DRIVER_MMIO_READ32);
        case TOK_DRIVER_MMIO_WRITE32:
            return emit_u8(compiler, BCVM_OP_DRIVER_MMIO_WRITE32);
        case TOK_DRIVER_SHOULD_STOP:
            return emit_u8(compiler, BCVM_OP_DRIVER_SHOULD_STOP);
        default:
            compiler->error = "unsupported driver API";
            return -1;
    }
}

static int parse_primary(PrismCompiler* compiler) {
    Token token = compiler->lexer.current;

    if (token.type == TOK_FILE_READ) {
        uint8_t path_type;

        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after file_read") != 0) {
            return -1;
        }

        path_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (path_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_read path must be string";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after file_read path") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_FILE_READ);
    }

    if (token.type == TOK_FILE_EXISTS) {
        uint8_t path_type;

        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after file_exists") != 0) {
            return -1;
        }

        path_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (path_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_exists path must be string";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after file_exists path") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_FILE_EXISTS);
    }

    if (token.type == TOK_READ_TEXT) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after read_text") != 0) {
            return -1;
        }

        if (compiler->lexer.current.type != TOK_RPAREN) {
            if (parse_expression(compiler) != 0) {
                return -1;
            }

            if (emit_u8(compiler, BCVM_OP_PRINT_STR_VAL) != 0) {
                return -1;
            }
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after read_text") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_READ_TEXT);
    }

    if (token.type == TOK_INPUT_LEN) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after input_len") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after input_len") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_INPUT_LEN);
    }

    if (token.type == TOK_INPUT_EQ) {
        Token str;
        uint16_t offset;
        uint16_t length;

        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after input_eq") != 0) {
            return -1;
        }

        if (compiler->lexer.current.type != TOK_STRING) {
            compiler->error = "input_eq requires string literal";
            return -1;
        }

        str = compiler->lexer.current;
        length = (uint16_t)string_length(str.text);
        if (add_data(compiler, str.text, length, &offset) != 0) {
            return -1;
        }

        next_token(compiler);

        if (expect(compiler, TOK_RPAREN, "expected ')' after input_eq") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_INPUT_EQ) != 0
            || emit_u16(compiler, offset) != 0
            || emit_u16(compiler, length) != 0) {
            return -1;
        }

        return 0;
    }

    if (token.type == TOK_STRING_LEN) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after string_len") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after string_len") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_STR_LEN) != 0) {
            return -1;
        }

        return 0;
    }

    if (token.type == TOK_STRING_EQ) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after string_eq") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' in string_eq") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after string_eq") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_STR_EQ);
    }

    if (token.type == TOK_INPUT_INT) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after input_int") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after input_int") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_READ_INT);
    }

    if (token.type == TOK_APP_SHOULD_QUIT) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after app_should_quit") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after app_should_quit") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_APP_SHOULD_QUIT);
    }

    if (token.type == TOK_DRIVER_SERIAL_WRITE
        || token.type == TOK_DRIVER_SERIAL_READ
        || token.type == TOK_DRIVER_PORT_READ8
        || token.type == TOK_DRIVER_PORT_WRITE8
        || token.type == TOK_DRIVER_MMIO_READ32
        || token.type == TOK_DRIVER_MMIO_WRITE32
        || token.type == TOK_DRIVER_SHOULD_STOP) {
        return parse_driver_builtin(compiler, token.type);
    }

    if (token.type == TOK_NUMBER) {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, (uint32_t)token.number) != 0) {
            return -1;
        }

        next_token(compiler);
        return compiler->error == 0 ? 0 : -1;
    }

    if (token.type == TOK_FLOAT_NUMBER) {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, (uint32_t)token.number) != 0) {
            return -1;
        }

        next_token(compiler);
        return compiler->error == 0 ? 0 : -1;
    }

    if (token.type == TOK_STRING) {
        uint16_t offset;
        uint16_t length = (uint16_t)string_length(token.text);
        uint32_t descriptor;

        if (add_data(compiler, token.text, length, &offset) != 0) {
            return -1;
        }

        descriptor = make_literal_string_descriptor(offset, length);
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, descriptor) != 0) {
            return -1;
        }

        next_token(compiler);
        return 0;
    }

    if (token_is_name(token.type)) {
        char name[PRISMCC_MAX_TOKEN_TEXT];
        Local* local;

        copy_string(name, token.text, sizeof(name));
        next_token(compiler);

        if (compiler->lexer.current.type == TOK_LPAREN) {
            return parse_call_after_name(compiler, name, 0);
        }

        local = find_local_entry(compiler, name);
        if (local == 0) {
            compiler->error = "unknown identifier";
            return -1;
        }

        if (compiler->lexer.current.type == TOK_DOT) {
            StructDef* def;
            int field_index;

            if (local->type != (uint8_t)LOCAL_TYPE_STRUCT || local->struct_index == 0xFFU) {
                compiler->error = "field access requires a struct variable";
                return -1;
            }

            def = &compiler->structs[local->struct_index];

            next_token(compiler);
            if (!token_is_name(compiler->lexer.current.type)) {
                compiler->error = "expected field name";
                return -1;
            }

            field_index = find_struct_field_index(def, compiler->lexer.current.text);
            if (field_index < 0) {
                compiler->error = "unknown struct field";
                return -1;
            }

            next_token(compiler);

            if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
                return -1;
            }

            if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, (uint32_t)field_index) != 0) {
                return -1;
            }

            return emit_u8(compiler, BCVM_OP_ARR_GET);
        }

        if (compiler->lexer.current.type == TOK_LBRACKET) {
            if (local->type != (uint8_t)LOCAL_TYPE_ARRAY) {
                compiler->error = "indexed access requires an array variable";
                return -1;
            }

            if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
                return -1;
            }

            next_token(compiler);
            if (parse_expression(compiler) != 0) {
                return -1;
            }

            if (expect(compiler, TOK_RBRACKET, "expected ']' after array index") != 0) {
                return -1;
            }

            return emit_u8(compiler, BCVM_OP_ARR_GET);
        }

        if (local->type == (uint8_t)LOCAL_TYPE_ARRAY) {
            compiler->error = "array value requires index access";
            return -1;
        }

        if (compiler->lexer.current.type == TOK_PLUSPLUS) {
            next_token(compiler);
            return emit_local_update(compiler, local, 1, 1, 1);
        }

        if (compiler->lexer.current.type == TOK_MINUSMINUS) {
            next_token(compiler);
            return emit_local_update(compiler, local, -1, 1, 1);
        }

        if (emit_u8(compiler, BCVM_OP_LOAD_LOCAL) != 0 || emit_u16(compiler, local->index) != 0) {
            return -1;
        }

        return 0;
    }

    if (token.type == TOK_LPAREN) {
        next_token(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        return expect(compiler, TOK_RPAREN, "expected ')' after expression");
    }

    compiler->error = "invalid expression";
    return -1;
}

static int parse_unary(PrismCompiler* compiler) {
    if (compiler->lexer.current.type == TOK_PLUS) {
        next_token(compiler);
        return parse_unary(compiler);
    }

    if (compiler->lexer.current.type == TOK_MINUS) {
        next_token(compiler);
        if (parse_unary(compiler) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_NEG);
    }

    if (compiler->lexer.current.type == TOK_PLUSPLUS) {
        return parse_prefix_update(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_MINUSMINUS) {
        return parse_prefix_update(compiler, -1);
    }

    return parse_primary(compiler);
}

static int parse_term(PrismCompiler* compiler) {
    uint8_t lhs_type = guess_expression_type(compiler);

    if (parse_unary(compiler) != 0) {
        return -1;
    }

    while (compiler->lexer.current.type == TOK_STAR
        || compiler->lexer.current.type == TOK_SLASH
        || compiler->lexer.current.type == TOK_PERCENT) {
        TokenType op = compiler->lexer.current.type;
        uint8_t rhs_type;
        next_token(compiler);

        rhs_type = guess_expression_type(compiler);

        if (parse_unary(compiler) != 0) {
            return -1;
        }

        if (lhs_type == (uint8_t)LOCAL_TYPE_FLOAT || rhs_type == (uint8_t)LOCAL_TYPE_FLOAT) {
            if (op == TOK_PERCENT) {
                compiler->error = "float modulo is not supported";
                return -1;
            }

            if (lhs_type != (uint8_t)LOCAL_TYPE_FLOAT || rhs_type != (uint8_t)LOCAL_TYPE_FLOAT) {
                compiler->error = "mixed int/float arithmetic is not supported";
                return -1;
            }

            if (emit_u8(compiler, op == TOK_STAR ? BCVM_OP_FMUL : BCVM_OP_FDIV) != 0) {
                return -1;
            }

            lhs_type = (uint8_t)LOCAL_TYPE_FLOAT;
        } else {
            if (op == TOK_STAR) {
                if (emit_u8(compiler, BCVM_OP_MUL) != 0) {
                    return -1;
                }
            } else if (op == TOK_SLASH) {
                if (emit_u8(compiler, BCVM_OP_DIV) != 0) {
                    return -1;
                }
            } else {
                if (emit_u8(compiler, BCVM_OP_MOD) != 0) {
                    return -1;
                }
            }

            lhs_type = (uint8_t)LOCAL_TYPE_INT;
        }

        if (compiler->error != 0) {
            return -1;
        }
    }

    return compiler->error == 0 ? 0 : -1;
}

static int parse_additive(PrismCompiler* compiler) {
    uint8_t lhs_type = guess_expression_type(compiler);

    if (parse_term(compiler) != 0) {
        return -1;
    }

    while (compiler->lexer.current.type == TOK_PLUS || compiler->lexer.current.type == TOK_MINUS) {
        TokenType op = compiler->lexer.current.type;
        uint8_t rhs_type;
        next_token(compiler);

        rhs_type = guess_expression_type(compiler);

        if (parse_term(compiler) != 0) {
            return -1;
        }

        if ((lhs_type == (uint8_t)LOCAL_TYPE_FLOAT || rhs_type == (uint8_t)LOCAL_TYPE_FLOAT)
            && (lhs_type != (uint8_t)LOCAL_TYPE_FLOAT || rhs_type != (uint8_t)LOCAL_TYPE_FLOAT)) {
            compiler->error = "mixed int/float arithmetic is not supported";
            return -1;
        }

        if (emit_u8(compiler, op == TOK_PLUS ? BCVM_OP_ADD : BCVM_OP_SUB) != 0) {
            return -1;
        }

        lhs_type = (lhs_type == (uint8_t)LOCAL_TYPE_FLOAT || rhs_type == (uint8_t)LOCAL_TYPE_FLOAT)
            ? (uint8_t)LOCAL_TYPE_FLOAT
            : (uint8_t)LOCAL_TYPE_INT;
    }

    return compiler->error == 0 ? 0 : -1;
}

static int parse_relational(PrismCompiler* compiler) {
    if (parse_additive(compiler) != 0) {
        return -1;
    }

    while (compiler->lexer.current.type == TOK_LT
        || compiler->lexer.current.type == TOK_LTE
        || compiler->lexer.current.type == TOK_GT
        || compiler->lexer.current.type == TOK_GTE) {
        TokenType op = compiler->lexer.current.type;
        next_token(compiler);

        if (parse_additive(compiler) != 0) {
            return -1;
        }

        if (op == TOK_LT) {
            if (emit_u8(compiler, BCVM_OP_LT) != 0) {
                return -1;
            }
        } else if (op == TOK_LTE) {
            if (emit_u8(compiler, BCVM_OP_LE) != 0) {
                return -1;
            }
        } else if (op == TOK_GT) {
            if (emit_u8(compiler, BCVM_OP_GT) != 0) {
                return -1;
            }
        } else {
            if (emit_u8(compiler, BCVM_OP_GE) != 0) {
                return -1;
            }
        }
    }

    return compiler->error == 0 ? 0 : -1;
}

static int parse_expression(PrismCompiler* compiler) {
    if (parse_relational(compiler) != 0) {
        return -1;
    }

    while (compiler->lexer.current.type == TOK_EQEQ || compiler->lexer.current.type == TOK_NEQ) {
        TokenType op = compiler->lexer.current.type;
        next_token(compiler);

        if (parse_relational(compiler) != 0) {
            return -1;
        }

        if (emit_u8(compiler, op == TOK_EQEQ ? BCVM_OP_EQ : BCVM_OP_NE) != 0) {
            return -1;
        }
    }

    return compiler->error == 0 ? 0 : -1;
}

static int parse_block(PrismCompiler* compiler, int* saw_return) {
    if (expect(compiler, TOK_LBRACE, "expected '{'") != 0) {
        return -1;
    }

    while (compiler->lexer.current.type != TOK_RBRACE && compiler->lexer.current.type != TOK_EOF) {
        if (parse_statement(compiler, saw_return) != 0) {
            return -1;
        }
    }

    return expect(compiler, TOK_RBRACE, "expected '}'");
}

static int parse_if_statement(PrismCompiler* compiler, int* saw_return) {
    uint32_t false_jump;

    next_token(compiler);
    if (expect(compiler, TOK_LPAREN, "expected '(' after if") != 0) {
        return -1;
    }

    if (parse_expression(compiler) != 0) {
        return -1;
    }

    if (expect(compiler, TOK_RPAREN, "expected ')' after if condition") != 0) {
        return -1;
    }

    if (emit_jump_placeholder(compiler, BCVM_OP_JMP_IF_ZERO, &false_jump) != 0) {
        return -1;
    }

    if (parse_statement(compiler, saw_return) != 0) {
        return -1;
    }

    if (compiler->lexer.current.type == TOK_ELSE) {
        uint32_t end_jump;

        if (emit_jump_placeholder(compiler, BCVM_OP_JMP, &end_jump) != 0) {
            return -1;
        }

        patch_u32_at(compiler, false_jump, compiler->code_size);

        next_token(compiler);
        if (parse_statement(compiler, saw_return) != 0) {
            return -1;
        }

        patch_u32_at(compiler, end_jump, compiler->code_size);
        return compiler->error == 0 ? 0 : -1;
    }

    patch_u32_at(compiler, false_jump, compiler->code_size);
    return compiler->error == 0 ? 0 : -1;
}

static int parse_while_statement(PrismCompiler* compiler, int* saw_return) {
    uint32_t cond_offset;
    uint32_t exit_jump;

    next_token(compiler);
    if (expect(compiler, TOK_LPAREN, "expected '(' after while") != 0) {
        return -1;
    }

    cond_offset = compiler->code_size;

    if (parse_expression(compiler) != 0) {
        return -1;
    }

    if (expect(compiler, TOK_RPAREN, "expected ')' after while condition") != 0) {
        return -1;
    }

    if (emit_jump_placeholder(compiler, BCVM_OP_JMP_IF_ZERO, &exit_jump) != 0) {
        return -1;
    }

    if (parse_statement(compiler, saw_return) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_JMP) != 0 || emit_u32(compiler, cond_offset) != 0) {
        return -1;
    }

    patch_u32_at(compiler, exit_jump, compiler->code_size);
    return compiler->error == 0 ? 0 : -1;
}

static int parse_for_statement(PrismCompiler* compiler, int* saw_return) {
    uint32_t cond_offset;
    uint32_t exit_jump;
    uint32_t body_jump;
    uint32_t inc_offset;
    uint32_t body_offset;

    next_token(compiler);
    if (expect(compiler, TOK_LPAREN, "expected '(' after for") != 0) {
        return -1;
    }

    if (compiler->lexer.current.type != TOK_SEMI) {
        if (parse_for_clause_item(compiler, 1) != 0) {
            return -1;
        }
    }

    if (expect(compiler, TOK_SEMI, "expected ';' after for init") != 0) {
        return -1;
    }

    cond_offset = compiler->code_size;

    if (compiler->lexer.current.type != TOK_SEMI) {
        if (parse_expression(compiler) != 0) {
            return -1;
        }
    } else {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, 1U) != 0) {
            return -1;
        }
    }

    if (expect(compiler, TOK_SEMI, "expected ';' after for condition") != 0) {
        return -1;
    }

    if (emit_jump_placeholder(compiler, BCVM_OP_JMP_IF_ZERO, &exit_jump) != 0) {
        return -1;
    }

    if (emit_jump_placeholder(compiler, BCVM_OP_JMP, &body_jump) != 0) {
        return -1;
    }

    inc_offset = compiler->code_size;

    if (compiler->lexer.current.type != TOK_RPAREN) {
        if (parse_for_clause_item(compiler, 0) != 0) {
            return -1;
        }
    }

    if (expect(compiler, TOK_RPAREN, "expected ')' after for clauses") != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_JMP) != 0 || emit_u32(compiler, cond_offset) != 0) {
        return -1;
    }

    body_offset = compiler->code_size;
    patch_u32_at(compiler, body_jump, body_offset);

    if (parse_statement(compiler, saw_return) != 0) {
        return -1;
    }

    if (emit_u8(compiler, BCVM_OP_JMP) != 0 || emit_u32(compiler, inc_offset) != 0) {
        return -1;
    }

    patch_u32_at(compiler, exit_jump, compiler->code_size);
    return compiler->error == 0 ? 0 : -1;
}

static int parse_statement(PrismCompiler* compiler, int* saw_return) {
    if (compiler->lexer.current.type == TOK_LBRACE) {
        return parse_block(compiler, saw_return);
    }

    if (compiler->lexer.current.type == TOK_INT) {
        return parse_int_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_INT8) {
        return parse_int8_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_INT16) {
        return parse_int16_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_INT32) {
        return parse_int32_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_UINT8) {
        return parse_uint8_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_UINT16) {
        return parse_uint16_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_UINT32) {
        return parse_uint32_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_CHAR_TYPE) {
        return parse_char_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_FLOAT_TYPE) {
        return parse_float_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_STRING_TYPE) {
        return parse_string_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_STRUCT) {
        return parse_struct_variable_declaration(compiler, 1);
    }

    if (compiler->lexer.current.type == TOK_PRINT) {
        uint8_t print_arg_type;

        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after print") != 0) {
            return -1;
        }

        print_arg_type = guess_expression_type(compiler);

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after print argument") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after print") != 0) {
            return -1;
        }

        if (print_arg_type == (uint8_t)LOCAL_TYPE_STRING) {
            if (emit_u8(compiler, BCVM_OP_PRINT_STR_VAL) != 0) {
                return -1;
            }
        } else if (print_arg_type == (uint8_t)LOCAL_TYPE_FLOAT) {
            if (emit_u8(compiler, BCVM_OP_PRINT_FLOAT) != 0) {
                return -1;
            }
        } else {
            if (emit_u8(compiler, BCVM_OP_PRINT_INT) != 0) {
                return -1;
            }
        }

        return emit_u8(compiler, BCVM_OP_PRINT_NL);
    }

    if (compiler->lexer.current.type == TOK_PRINT_INT) {
        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after print_int") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after print_int argument") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after print_int") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_PRINT_INT) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_PRINT_NL);
    }

    if (compiler->lexer.current.type == TOK_PRINT_CHAR) {
        uint8_t char_arg_type;

        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after print_char") != 0) {
            return -1;
        }

        char_arg_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (char_arg_type != (uint8_t)LOCAL_TYPE_INT) {
            compiler->error = "print_char argument must be int";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after print_char argument") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after print_char") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_PRINT_CHAR);
    }

    if (compiler->lexer.current.type == TOK_PRINT_COLOR) {
        uint8_t message_type;

        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after print_color") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' after color expression") != 0) {
            return -1;
        }

        message_type = guess_expression_type(compiler);

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (message_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "print_color message must be string";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after print_color arguments") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after print_color") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_PRINT_COLOR_STR_VAL) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_PRINT_NL);
    }

    if (compiler->lexer.current.type == TOK_READ_TEXT) {
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after read_text") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_POP);
    }

    if (compiler->lexer.current.type == TOK_FILE_WRITE) {
        uint8_t path_type;
        uint8_t text_type;

        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after file_write") != 0) {
            return -1;
        }

        path_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (path_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_write path must be string";
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' after file_write path") != 0) {
            return -1;
        }

        text_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (text_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_write text must be string";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after file_write arguments") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after file_write") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_FILE_WRITE) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_POP);
    }

    if (compiler->lexer.current.type == TOK_FILE_APPEND) {
        uint8_t path_type;
        uint8_t text_type;

        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after file_append") != 0) {
            return -1;
        }

        path_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (path_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_append path must be string";
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' after file_append path") != 0) {
            return -1;
        }

        text_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (text_type != (uint8_t)LOCAL_TYPE_STRING) {
            compiler->error = "file_append text must be string";
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after file_append arguments") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after file_append") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_FILE_APPEND) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_POP);
    }

    if (compiler->lexer.current.type == TOK_SET_FULLSCREEN) {
        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after set_fullscreen") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after set_fullscreen") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after set_fullscreen") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_SET_FULLSCREEN);
    }

    if (compiler->lexer.current.type == TOK_DRAW_PIXEL) {
        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after draw_pixel") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' after draw_pixel x") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_COMMA, "expected ',' after draw_pixel y") != 0) {
            return -1;
        }

        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after draw_pixel arguments") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after draw_pixel") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_DRAW_PIXEL);
    }

    if (compiler->lexer.current.type == TOK_APP_EXIT) {
        next_token(compiler);
        if (expect(compiler, TOK_LPAREN, "expected '(' after app_exit") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after app_exit") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after app_exit") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_APP_EXIT);
    }

    if (compiler->lexer.current.type == TOK_PRINT_INPUT) {
        next_token(compiler);

        if (expect(compiler, TOK_LPAREN, "expected '(' after print_input") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_RPAREN, "expected ')' after print_input") != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after print_input") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_PRINT_INPUT) != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_PRINT_NL);
    }

    if (compiler->lexer.current.type == TOK_IF) {
        return parse_if_statement(compiler, saw_return);
    }

    if (compiler->lexer.current.type == TOK_WHILE) {
        return parse_while_statement(compiler, saw_return);
    }

    if (compiler->lexer.current.type == TOK_FOR) {
        return parse_for_statement(compiler, saw_return);
    }

    if (compiler->lexer.current.type == TOK_RETURN) {
        uint8_t return_expr_type;

        next_token(compiler);

        if (compiler->current_function_return_type == (uint8_t)LOCAL_TYPE_VOID) {
            if (compiler->lexer.current.type != TOK_SEMI) {
                compiler->error = "void function cannot return a value";
                return -1;
            }

            if (expect(compiler, TOK_SEMI, "expected ';' after return") != 0) {
                return -1;
            }

            if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, 0U) != 0) {
                return -1;
            }

            if (emit_u8(compiler, BCVM_OP_RET) != 0) {
                return -1;
            }

            *saw_return = 1;
            return 0;
        }

        if (compiler->lexer.current.type == TOK_SEMI) {
            compiler->error = "non-void function must return a value";
            return -1;
        }

        return_expr_type = guess_expression_type(compiler);
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (!types_compatible(compiler->current_function_return_type, return_expr_type)) {
            compiler->error = "return type mismatch";
            return -1;
        }

        if (compiler->current_function_return_type == (uint8_t)LOCAL_TYPE_FLOAT
            && return_expr_type == (uint8_t)LOCAL_TYPE_INT) {
            if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0
                || emit_u32(compiler, PRISMCC_FLOAT_SCALE) != 0
                || emit_u8(compiler, BCVM_OP_MUL) != 0) {
                return -1;
            }
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after return") != 0) {
            return -1;
        }

        if (emit_u8(compiler, BCVM_OP_RET) != 0) {
            return -1;
        }

        *saw_return = 1;
        return 0;
    }

    if (compiler->lexer.current.type == TOK_PLUSPLUS || compiler->lexer.current.type == TOK_MINUSMINUS) {
        if (parse_expression(compiler) != 0) {
            return -1;
        }

        if (expect(compiler, TOK_SEMI, "expected ';' after increment/decrement") != 0) {
            return -1;
        }

        return emit_u8(compiler, BCVM_OP_POP);
    }

    if (token_is_name(compiler->lexer.current.type)) {
        char name[PRISMCC_MAX_TOKEN_TEXT];
        Local* local;

        copy_string(name, compiler->lexer.current.text, sizeof(name));
        next_token(compiler);

        local = find_local_entry(compiler, name);

        if (compiler->lexer.current.type == TOK_ASSIGN) {
            if (parse_assignment_after_name(compiler, name) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after assignment");
        }

        if (compiler->lexer.current.type == TOK_DOT) {
            if (parse_struct_store_after_name(compiler, name) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after field assignment");
        }

        if (compiler->lexer.current.type == TOK_LBRACKET) {
            if (parse_array_store_after_name(compiler, name) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after array assignment");
        }

        if (compiler->lexer.current.type == TOK_LPAREN) {
            if (parse_call_after_name(compiler, name, 1) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after function call");
        }

        if (compiler->lexer.current.type == TOK_PLUSPLUS) {
            next_token(compiler);

            if (emit_local_update(compiler, local, 1, 0, 0) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after increment");
        }

        if (compiler->lexer.current.type == TOK_MINUSMINUS) {
            next_token(compiler);

            if (emit_local_update(compiler, local, -1, 0, 0) != 0) {
                return -1;
            }

            return expect(compiler, TOK_SEMI, "expected ';' after decrement");
        }

        compiler->error = "unsupported statement";
        return -1;
    }

    compiler->error = "unsupported statement";
    return -1;
}

static int parse_function(PrismCompiler* compiler) {
    char function_name[PRISMCC_MAX_TOKEN_TEXT];
    uint8_t param_types[PRISMCC_MAX_PARAMS] = {0};
    uint8_t param_count = 0;
    int saw_return = 0;
    TokenType return_type = compiler->lexer.current.type;
    LocalType return_local_type = LOCAL_TYPE_INT;

    if (!token_is_decl_type(return_type)) {
        compiler->error = "expected valid return type at function start";
        return -1;
    }

    return_local_type = token_to_local_type(return_type);

    if (expect(compiler, return_type, "expected function return type") != 0) {
        return -1;
    }

    if (!token_is_name(compiler->lexer.current.type)) {
        compiler->error = "expected function name";
        return -1;
    }

    copy_string(function_name, compiler->lexer.current.text, sizeof(function_name));
    next_token(compiler);

    if (expect(compiler, TOK_LPAREN, "expected '(' after function name") != 0) {
        return -1;
    }

    compiler->local_count = 0;
    compiler->current_function_return_type = (uint8_t)return_local_type;

    if (compiler->lexer.current.type != TOK_RPAREN) {
        while (1) {
            char param_name[PRISMCC_MAX_TOKEN_TEXT];
            uint16_t ignored_index;
            LocalType param_type;
            TokenType param_token = compiler->lexer.current.type;

            if (!token_is_decl_type(param_token)
                || param_token == TOK_VOID) {
                compiler->error = "expected numeric, char, string or float parameter type";
                return -1;
            }

            if (expect(compiler, param_token, "expected parameter type") != 0) {
                return -1;
            }

            param_type = token_to_local_type(param_token);

            if (!token_is_name(compiler->lexer.current.type)) {
                compiler->error = "expected parameter name";
                return -1;
            }

            copy_string(param_name, compiler->lexer.current.text, sizeof(param_name));
            next_token(compiler);

            if (add_local(compiler, param_name, param_type, &ignored_index) != 0) {
                return -1;
            }

            param_types[param_count] = (uint8_t)param_type;
            param_count++;
            if (compiler->lexer.current.type != TOK_COMMA) {
                break;
            }

            next_token(compiler);
        }
    }

    if (expect(compiler, TOK_RPAREN, "expected ')' after parameter list") != 0) {
        return -1;
    }

    if (add_function(compiler, function_name, compiler->code_size, param_count, return_local_type, param_types) != 0) {
        return -1;
    }

    if (string_equals(function_name, "main")) {
        compiler->main_entry = compiler->code_size;
    }

    if (parse_block(compiler, &saw_return) != 0) {
        return -1;
    }

    if (!saw_return) {
        if (emit_u8(compiler, BCVM_OP_PUSH_I32) != 0 || emit_u32(compiler, 0U) != 0 || emit_u8(compiler, BCVM_OP_RET) != 0) {
            return -1;
        }
    }

    return 0;
}

static int resolve_call_patches(PrismCompiler* compiler) {
    for (uint32_t i = 0; i < compiler->call_patch_count; i++) {
        int function_index = find_function_overload(compiler,
            compiler->call_patches[i].callee,
            compiler->call_patches[i].arg_count,
            compiler->call_patches[i].arg_types);
        uint32_t function_uindex;

        if (function_index < 0) {
            compiler->error = "call to unknown overload";
            return -1;
        }

        function_uindex = (uint32_t)function_index;

        if (compiler->call_patches[i].expects_value != 0U
            && compiler->functions[function_uindex].return_type == (uint8_t)LOCAL_TYPE_VOID) {
            compiler->error = "void function call cannot be used as a value";
            return -1;
        }

        patch_u32_at(compiler,
            compiler->call_patches[i].code_immediate_offset,
            compiler->functions[function_uindex].entry_offset);

        if (compiler->error != 0) {
            return -1;
        }
    }

    return 0;
}

static int validate_driver_contract(PrismCompiler* compiler) {
    int init_index = find_function_overload(compiler, "driver_init", 0U, 0);
    int poll_index = find_function_overload(compiler, "driver_poll", 0U, 0);
    int shutdown_index = find_function_overload(compiler, "driver_shutdown", 0U, 0);
    int main_index = find_function_overload(compiler, "main", 0U, 0);

    if (init_index < 0 || poll_index < 0 || shutdown_index < 0 || main_index < 0) {
        compiler->error = "driver packages require driver_init(), driver_poll(), driver_shutdown(), and main()";
        return -1;
    }

    if (compiler->functions[(uint32_t)init_index].return_type != (uint8_t)LOCAL_TYPE_INT) {
        compiler->error = "driver_init() must return int";
        return -1;
    }

    if (compiler->functions[(uint32_t)main_index].return_type != (uint8_t)LOCAL_TYPE_INT) {
        compiler->error = "driver main() must return int";
        return -1;
    }

    return 0;
}

static int parse_program(PrismCompiler* compiler) {
    while (compiler->lexer.current.type != TOK_EOF) {
        if (compiler->lexer.current.type == TOK_STRUCT) {
            if (parse_struct_definition(compiler) != 0) {
                return -1;
            }

            continue;
        }

        if (!token_is_decl_type(compiler->lexer.current.type)) {
            compiler->error = "top-level items must use supported method return types";
            return -1;
        }

        if (parse_function(compiler) != 0) {
            return -1;
        }
    }

    if (compiler->main_entry == 0xFFFFFFFFU) {
        compiler->error = "missing main function";
        return -1;
    }

    if (compiler->driver_mode && validate_driver_contract(compiler) != 0) {
        return -1;
    }

    return resolve_call_patches(compiler);
}

static void write_u16le(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_u32le(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static int path_is_driver_package(const char* path) {
    uint32_t length = string_length(path);
    const char* suffix = ".pdrv";

    if (length >= 4U) {
        const char* short_suffix = ".pdr";
        int matches_short_suffix = 1;

        for (uint32_t index = 0; index < 4U; index++) {
            char actual = path[length - 4U + index];
            if (actual >= 'A' && actual <= 'Z') {
                actual = (char)(actual - 'A' + 'a');
            }
            if (actual != short_suffix[index]) {
                matches_short_suffix = 0;
                break;
            }
        }
        if (matches_short_suffix) {
            return 1;
        }
    }

    if (length < 5U) {
        return 0;
    }

    for (uint32_t index = 0; index < 5U; index++) {
        char actual = path[length - 5U + index];
        char expected = suffix[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = (char)(actual - 'A' + 'a');
        }
        if (actual != expected) {
            return 0;
        }
    }

    return 1;
}

int prismcc_compile_file(const char* input_abs_path, const char* output_abs_path, char* out_error, uint32_t out_error_capacity) {
    uint32_t source_size = 0;
    uint32_t vm_image_size;
    uint32_t total_size;

    if (input_abs_path == 0 || output_abs_path == 0) {
        set_error(out_error, out_error_capacity, "invalid compiler paths");
        return -1;
    }

    source_buffer[0] = '\0';
    if (preprocess_file_recursive(input_abs_path, 0U, &source_size, out_error, out_error_capacity) != 0) {
        return -1;
    }

    compiler.lexer.src = source_buffer;
    compiler.lexer.length = source_size;
    compiler.lexer.position = 0;
    compiler.lexer.line = 1U;
    compiler.lexer.current.type = TOK_EOF;
    compiler.code_size = 0;
    compiler.data_size = 0;
    compiler.local_count = 0;
    compiler.function_count = 0;
    compiler.struct_count = 0;
    compiler.call_patch_count = 0;
    compiler.main_entry = 0xFFFFFFFFU;
    compiler.driver_mode = (uint8_t)path_is_driver_package(output_abs_path);
    compiler.error = 0;

    next_token(&compiler);

    if (compiler.error == 0 && parse_program(&compiler) != 0) {
        if (compiler.error == 0) {
            compiler.error = "compile failed";
        }
    }

    if (compiler.error != 0) {
        ERROR_LOG("in-OS prismcc compile failed");
        set_error_with_line(out_error, out_error_capacity, compiler.lexer.current.line, compiler.error);
        return -1;
    }

    vm_image_size = BCVM_IMAGE_HEADER_SIZE + compiler.code_size + compiler.data_size;
    total_size = PRISM_APP_HEADER_SIZE + vm_image_size;

    write_u32le(&output_buffer[0], PRISM_APP_MAGIC);
    write_u16le(&output_buffer[4], PRISM_APP_FORMAT_VERSION);
    write_u16le(&output_buffer[6], compiler.driver_mode ? PRISM_APP_FLAG_DRIVER : 0U);
    write_u32le(&output_buffer[8], 0U);
    write_u32le(&output_buffer[12], vm_image_size);
    write_u32le(&output_buffer[16], 0U);

    write_u32le(&output_buffer[PRISM_APP_HEADER_SIZE + 0U], BCVM_MAGIC);
    write_u16le(&output_buffer[PRISM_APP_HEADER_SIZE + 4U], BCVM_VERSION);
    write_u16le(&output_buffer[PRISM_APP_HEADER_SIZE + 6U], 0U);
    write_u32le(&output_buffer[PRISM_APP_HEADER_SIZE + 8U], compiler.code_size);
    write_u32le(&output_buffer[PRISM_APP_HEADER_SIZE + 12U], compiler.data_size);
    write_u32le(&output_buffer[PRISM_APP_HEADER_SIZE + 16U], compiler.main_entry);

    for (uint32_t i = 0; i < compiler.code_size; i++) {
        output_buffer[PRISM_APP_HEADER_SIZE + BCVM_IMAGE_HEADER_SIZE + i] = compiler.code[i];
    }

    for (uint32_t i = 0; i < compiler.data_size; i++) {
        output_buffer[PRISM_APP_HEADER_SIZE + BCVM_IMAGE_HEADER_SIZE + compiler.code_size + i] = compiler.data[i];
    }

    if (vfs_write_file(output_abs_path, (const char*)output_buffer, total_size, 0) != 0) {
        set_error(out_error, out_error_capacity, "failed to write app output");
        return -1;
    }

    DEBUG_LOG("in-OS prismcc compile completed");
    set_error(out_error, out_error_capacity, "ok");
    return 0;
}
