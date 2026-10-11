#include "apps/bank_app.h"

#include <stdint.h>

#include "debug/log.h"
#include "display/console.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"

#define BANK_DIR_PATH "/DATA/BANK"
#define BANK_DB_PATH "/DATA/BANK/ACCOUNTS.DB"
#define BANK_MAX_ACCOUNTS 64U
#define BANK_NAME_MAX 31U
#define BANK_PASS_MAX 31U
#define BANK_FILE_BUFFER 8192U

typedef struct {
    char username[BANK_NAME_MAX + 1U];
    char password[BANK_PASS_MAX + 1U];
    int32_t balance;
} BankAccount;

static BankAccount bank_accounts[BANK_MAX_ACCOUNTS];
static uint32_t bank_account_count = 0;
static char bank_file_buffer[BANK_FILE_BUFFER + 1U];

static void copy_limited(char* dst, uint32_t capacity, const char* src) {
    uint32_t i = 0;

    if (capacity == 0U) {
        return;
    }

    while (src[i] != '\0' && i + 1U < capacity) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
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

static uint32_t string_length(const char* value) {
    uint32_t len = 0;

    while (value[len] != '\0') {
        len++;
    }

    return len;
}

static int parse_i32(const char* text, int32_t* out_value) {
    uint32_t i = 0;
    int sign = 1;
    int32_t value = 0;

    if (text == 0 || out_value == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[i] == '-') {
        sign = -1;
        i++;
    }

    if (text[i] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }

        value = value * 10 + (int32_t)(text[i] - '0');
        i++;
    }

    *out_value = value * sign;
    return 0;
}

static void write_i32(int32_t value) {
    if (value < 0) {
        console_write_char('-');
        console_write_uint((unsigned int)(-value));
        return;
    }

    console_write_uint((unsigned int)value);
}

static void erase_last_char(void) {
    int x = console_get_x();
    int y = console_get_y();

    if (x <= 0) {
        return;
    }

    console_set_cursor(x - 1, y);
    console_write_char(' ');
    console_set_cursor(x - 1, y);
}

static int read_line(const char* prompt, char* out, uint32_t capacity, int allow_empty) {
    uint32_t len = 0;

    if (out == 0 || capacity == 0U) {
        return -1;
    }

    console_write(prompt);
    out[0] = '\0';

    while (1) {
        KeyEvent event = keyboard_read_event();

        if (event.type == KEY_EVENT_ENTER) {
            console_write_char('\n');
            out[len] = '\0';
            if (!allow_empty && len == 0U) {
                return -1;
            }
            return 0;
        }

        if (event.type == KEY_EVENT_BACKSPACE) {
            if (len > 0U) {
                len--;
                out[len] = '\0';
                erase_last_char();
            }
            continue;
        }

        if (event.type != KEY_EVENT_CHARACTER) {
            continue;
        }

        if (event.character >= ' ' && event.character <= '~') {
            if (len + 1U < capacity) {
                out[len++] = event.character;
                out[len] = '\0';
                console_write_char(event.character);
            }
        }
    }
}

static int read_int_prompt(const char* prompt, int32_t* out_value) {
    char input[32];

    if (read_line(prompt, input, sizeof(input), 0) != 0) {
        return -1;
    }

    return parse_i32(input, out_value);
}

static int append_text(char* out, uint32_t capacity, uint32_t* io_pos, const char* text) {
    uint32_t pos = *io_pos;
    uint32_t i = 0;

    while (text[i] != '\0') {
        if (pos + 1U >= capacity) {
            return -1;
        }

        out[pos++] = text[i++];
    }

    out[pos] = '\0';
    *io_pos = pos;
    return 0;
}

static int append_i32(char* out, uint32_t capacity, uint32_t* io_pos, int32_t value) {
    char reversed[16];
    uint32_t count = 0;
    uint32_t pos = *io_pos;
    int32_t temp = value;

    if (temp == 0) {
        if (pos + 2U >= capacity) {
            return -1;
        }

        out[pos++] = '0';
        out[pos] = '\0';
        *io_pos = pos;
        return 0;
    }

    if (temp < 0) {
        if (pos + 2U >= capacity) {
            return -1;
        }

        out[pos++] = '-';
        temp = -temp;
    }

    while (temp > 0 && count < sizeof(reversed)) {
        reversed[count++] = (char)('0' + (temp % 10));
        temp /= 10;
    }

    while (count > 0) {
        if (pos + 1U >= capacity) {
            return -1;
        }

        out[pos++] = reversed[--count];
    }

    out[pos] = '\0';
    *io_pos = pos;
    return 0;
}

static int ensure_bank_storage(void) {
    int is_dir = 0;

    if (vfs_path_is_dir(BANK_DIR_PATH, &is_dir) == 0) {
        return is_dir ? 0 : -1;
    }

    return vfs_mkdir(BANK_DIR_PATH);
}

static int find_account_index(const char* username) {
    for (uint32_t i = 0; i < bank_account_count; i++) {
        if (string_equals(bank_accounts[i].username, username)) {
            return (int)i;
        }
    }

    return -1;
}

static int load_accounts(void) {
    uint32_t size = 0;
    uint32_t pos = 0;

    bank_account_count = 0;

    if (vfs_read_file(BANK_DB_PATH, bank_file_buffer, BANK_FILE_BUFFER, &size) != 0) {
        return 0;
    }

    if (size > BANK_FILE_BUFFER) {
        return -1;
    }

    bank_file_buffer[size] = '\0';

    while (pos < size && bank_account_count < BANK_MAX_ACCOUNTS) {
        char username[BANK_NAME_MAX + 1U];
        char password[BANK_PASS_MAX + 1U];
        char balance_text[16];
        uint32_t ulen = 0;
        uint32_t plen = 0;
        uint32_t blen = 0;
        int32_t balance = 0;

        while (pos < size && (bank_file_buffer[pos] == '\n' || bank_file_buffer[pos] == '\r')) {
            pos++;
        }

        if (pos >= size) {
            break;
        }

        while (pos < size && bank_file_buffer[pos] != '|' && bank_file_buffer[pos] != '\n' && ulen < BANK_NAME_MAX) {
            username[ulen++] = bank_file_buffer[pos++];
        }
        username[ulen] = '\0';

        if (pos >= size || bank_file_buffer[pos] != '|') {
            return -1;
        }
        pos++;

        while (pos < size && bank_file_buffer[pos] != '|' && bank_file_buffer[pos] != '\n' && plen < BANK_PASS_MAX) {
            password[plen++] = bank_file_buffer[pos++];
        }
        password[plen] = '\0';

        if (pos >= size || bank_file_buffer[pos] != '|') {
            return -1;
        }
        pos++;

        while (pos < size && bank_file_buffer[pos] != '\n' && blen + 1U < sizeof(balance_text)) {
            balance_text[blen++] = bank_file_buffer[pos++];
        }
        balance_text[blen] = '\0';

        if (parse_i32(balance_text, &balance) != 0 || balance < 0) {
            return -1;
        }

        copy_limited(bank_accounts[bank_account_count].username, sizeof(bank_accounts[bank_account_count].username), username);
        copy_limited(bank_accounts[bank_account_count].password, sizeof(bank_accounts[bank_account_count].password), password);
        bank_accounts[bank_account_count].balance = balance;
        bank_account_count++;

        while (pos < size && bank_file_buffer[pos] != '\n') {
            pos++;
        }
        if (pos < size && bank_file_buffer[pos] == '\n') {
            pos++;
        }
    }

    return 0;
}

static int save_accounts(void) {
    uint32_t pos = 0;

    bank_file_buffer[0] = '\0';
    for (uint32_t i = 0; i < bank_account_count; i++) {
        if (append_text(bank_file_buffer, sizeof(bank_file_buffer), &pos, bank_accounts[i].username) != 0
            || append_text(bank_file_buffer, sizeof(bank_file_buffer), &pos, "|") != 0
            || append_text(bank_file_buffer, sizeof(bank_file_buffer), &pos, bank_accounts[i].password) != 0
            || append_text(bank_file_buffer, sizeof(bank_file_buffer), &pos, "|") != 0
            || append_i32(bank_file_buffer, sizeof(bank_file_buffer), &pos, bank_accounts[i].balance) != 0
            || append_text(bank_file_buffer, sizeof(bank_file_buffer), &pos, "\n") != 0) {
            return -1;
        }
    }

    return vfs_write_file(BANK_DB_PATH, bank_file_buffer, pos, 0);
}

static void print_main_menu(void) {
    console_writeln("");
    console_writeln("=== PrismOS Banking ===");
    console_writeln("1) Create account");
    console_writeln("2) Login");
    console_writeln("3) Exit");
}

static void print_account_menu(const char* username) {
    console_writeln("");
    console_write("Logged in as: ");
    console_writeln(username);
    console_writeln("1) Show balance");
    console_writeln("2) Deposit");
    console_writeln("3) Withdraw");
    console_writeln("4) Logout");
}

static void create_account_flow(void) {
    char username[BANK_NAME_MAX + 1U];
    char password[BANK_PASS_MAX + 1U];

    if (bank_account_count >= BANK_MAX_ACCOUNTS) {
        console_writeln("Account limit reached");
        return;
    }

    if (read_line("New username: ", username, sizeof(username), 0) != 0) {
        console_writeln("Username required");
        return;
    }

    if (string_length(username) < 3U) {
        console_writeln("Username too short");
        return;
    }

    if (find_account_index(username) >= 0) {
        console_writeln("Username already exists");
        return;
    }

    if (read_line("New password: ", password, sizeof(password), 0) != 0) {
        console_writeln("Password required");
        return;
    }

    copy_limited(bank_accounts[bank_account_count].username, sizeof(bank_accounts[bank_account_count].username), username);
    copy_limited(bank_accounts[bank_account_count].password, sizeof(bank_accounts[bank_account_count].password), password);
    bank_accounts[bank_account_count].balance = 0;
    bank_account_count++;

    if (save_accounts() != 0) {
        bank_account_count--;
        console_writeln("Failed to save account");
        return;
    }

    console_writeln("Account created");
}

static void account_session(uint32_t account_index) {
    while (1) {
        int32_t choice = 0;

        print_account_menu(bank_accounts[account_index].username);
        if (read_int_prompt("Choose: ", &choice) != 0) {
            console_writeln("Invalid choice");
            continue;
        }

        if (choice == 1) {
            console_write("Balance: ");
            write_i32(bank_accounts[account_index].balance);
            console_writeln("");
            continue;
        }

        if (choice == 2) {
            int32_t amount = 0;
            if (read_int_prompt("Deposit amount: ", &amount) != 0 || amount <= 0) {
                console_writeln("Invalid amount");
                continue;
            }

            bank_accounts[account_index].balance += amount;
            if (save_accounts() != 0) {
                bank_accounts[account_index].balance -= amount;
                console_writeln("Save failed");
                continue;
            }

            console_writeln("Deposit successful");
            continue;
        }

        if (choice == 3) {
            int32_t amount = 0;
            if (read_int_prompt("Withdraw amount: ", &amount) != 0 || amount <= 0) {
                console_writeln("Invalid amount");
                continue;
            }

            if (amount > bank_accounts[account_index].balance) {
                console_writeln("Insufficient funds");
                continue;
            }

            bank_accounts[account_index].balance -= amount;
            if (save_accounts() != 0) {
                bank_accounts[account_index].balance += amount;
                console_writeln("Save failed");
                continue;
            }

            console_writeln("Withdrawal successful");
            continue;
        }

        if (choice == 4) {
            console_writeln("Logged out");
            return;
        }

        console_writeln("Unknown choice");
    }
}

static void login_flow(void) {
    char username[BANK_NAME_MAX + 1U];
    char password[BANK_PASS_MAX + 1U];
    int index;

    if (read_line("Username: ", username, sizeof(username), 0) != 0) {
        console_writeln("Username required");
        return;
    }

    if (read_line("Password: ", password, sizeof(password), 0) != 0) {
        console_writeln("Password required");
        return;
    }

    index = find_account_index(username);
    if (index < 0) {
        console_writeln("Account not found");
        return;
    }

    if (!string_equals(bank_accounts[index].password, password)) {
        console_writeln("Invalid credentials");
        return;
    }

    account_session((uint32_t)index);
}

int bank_app_run(const char* args) {
    (void)args;

    if (ensure_bank_storage() != 0) {
        ERROR_LOG("bank app failed to initialize storage directory");
        console_writeln("Bank storage initialization failed");
        return -1;
    }

    if (load_accounts() != 0) {
        ERROR_LOG("bank app failed to load account database");
        console_writeln("Bank database is corrupted");
        return -1;
    }

    while (1) {
        int32_t choice = 0;

        print_main_menu();
        if (read_int_prompt("Choose: ", &choice) != 0) {
            console_writeln("Invalid choice");
            continue;
        }

        if (choice == 1) {
            create_account_flow();
            continue;
        }

        if (choice == 2) {
            login_flow();
            continue;
        }

        if (choice == 3) {
            console_writeln("Exiting banking app");
            return 0;
        }

        console_writeln("Unknown choice");
    }
}