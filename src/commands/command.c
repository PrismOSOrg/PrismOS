#include "command.h"

#include "comport/comport.h"
#include "debug/log.h"
#include "display/console.h"
#include "platform/io.h"
#include "platform/system.h"
#include "filesystem/blockdev.h"
#include "filesystem/vfs.h"
#include "filesystem/partition_manager.h"
#include "apps/app_manager.h"
#include "apps/help_app.h"
#include "apps/partition_manager_app.h"
#include "apps/prismcc_runtime.h"

#define COMMAND_PATH_CAPACITY 128
#define COMMAND_TOKEN_CAPACITY 64
#define COMMAND_TEXT_CAPACITY 512
#define COMMAND_MOVE_BUFFER_CAPACITY 4096

typedef void (*CommandHandler)(const char* arguments);

typedef struct {
    command_help_entry_t help;
    CommandHandler handler;
} Command;

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

static const char* skip_spaces(const char* text) {
    while (*text == ' ') {
        text++;
    }

    return text;
}

static unsigned int string_length(const char* text) {
    unsigned int len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

static void command_help(const char* arguments);
static void command_clear(const char* arguments);
static void command_echo(const char* arguments);
static void command_about(const char* arguments);
static void command_reboot(const char* arguments);
static void command_shutdown(const char* arguments);
static void command_comport(const char* arguments);
static void command_ls(const char* arguments);
static void command_cd(const char* arguments);
static void command_touch(const char* arguments);
static void command_mkdir(const char* arguments);
static void command_rm(const char* arguments);
static void command_rmdir(const char* arguments);
static void command_delete(const char* arguments);
static void command_cat(const char* arguments);
static void command_write(const char* arguments);
static void command_append(const char* arguments);
static void command_edit(const char* arguments);
static void command_ide(const char* arguments);
static void command_app_run(const char* arguments);
static void command_cc(const char* arguments);
static void command_driver_run(const char* arguments);
static int parse_drive_index(const char* text, uint32_t* out_index);
static void command_mv(const char* arguments);
static void command_partitions(const char* arguments);
static void command_drive(const char* arguments);

static char command_cwd[COMMAND_PATH_CAPACITY] = "/";

typedef struct {
    int count;
} LsCommandContext;

static int command_ls_visit(const vfs_entry_t* entry, void* context) {
    LsCommandContext* ls_context = (LsCommandContext*)context;

    console_write(entry->is_directory ? "[D] " : "[F] ");
    console_write(entry->name);
    if (!entry->is_directory) {
        console_write(" (");
        console_write_uint(entry->size);
        console_write(" bytes)");
    }
    console_write_char('\n');

    ls_context->count++;
    return 0;
}

static void copy_string_limited(char* destination, const char* source, unsigned int capacity) {
    unsigned int index = 0;

    if (capacity == 0U) {
        return;
    }

    while (source[index] != '\0' && index < (capacity - 1U)) {
        destination[index] = source[index];
        index++;
    }

    destination[index] = '\0';
}

static int parse_token(const char* input, char* token, unsigned int token_capacity, const char** remainder) {
    const char* cursor = skip_spaces(input);
    unsigned int length = 0;

    if (*cursor == '\0') {
        return -1;
    }

    while (*cursor != '\0' && *cursor != ' ') {
        if (length >= (token_capacity - 1U)) {
            return -1;
        }

        token[length++] = *cursor;
        cursor++;
    }

    token[length] = '\0';
    *remainder = skip_spaces(cursor);
    return 0;
}

static int resolve_to_absolute_path(const char* input_path, char* output_path, unsigned int output_capacity) {
    return vfs_normalize_path(command_cwd, input_path, output_path, output_capacity);
}

// One registry powers execution and help output, so adding commands stays local.
static const Command commands[] = {
    {{"about", "Shows the PrismOS version, execution mode, and detected memory information.", "about", "None.", "about"}, command_about},
    {{"app-run", "Loads and runs a Prism app package from the active drive. Optional trailing text is passed to the app runtime.", "app-run <path> [args]", "path: app package path. args: optional argument text passed to the app.", "app-run /APPS/BANK.APP"}, command_app_run},
    {{"append", "Adds text to the end of an existing file. The text is stored as provided; this command does not add a newline.", "append <path> <text>", "path: destination file path. text: all remaining text after the path token.", "append /NOTES.TXT More text"}, command_append},
    {{"cat", "Reads a text file and prints its contents. Output is limited by the shell command's text buffer.", "cat <path>", "path: file path to read.", "cat /README.TXT"}, command_cat},
    {{"cc", "Compiles supported PrismCC source into an app package, or into a cooperative driver package when the output uses the .pdr suffix.", "cc <input.c> <output.app|output.pdr>", "input.c: source file path. output.app: app destination. output.pdr: driver destination; requires the driver lifecycle functions.", "cc /SERDRV.C /DRIVERS/SERIAL.PDR"}, command_cc},
    {{"cd", "Changes the shell's current directory. Relative paths resolve from the current directory; drive switching returns to the root.", "cd <path>", "path: existing directory path.", "cd /DATA"}, command_cd},
    {{"clear", "Clears the visible console without changing the current directory or filesystem state.", "clear", "None.", "clear"}, command_clear},
    {{"comport", "Sends a line of text through the COM1 serial port, useful for external serial terminals and diagnostics.", "comport <text>", "text: message to transmit; spaces are included.", "comport test message"}, command_comport},
    {{"delete", "Removes a file or an empty directory. Non-empty directories cannot be removed.", "delete <path>", "path: existing file or empty directory path.", "delete /OLD.TXT"}, command_delete},
    {{"drive", "Lists physical ATA drives and capacities, or switches to a drive and mounts its first supported FAT32 volume. Partition slots are selected with partitions mount.", "drive [index]", "index: optional zero-based physical drive number shown by drive; it is not an MBR partition slot. Switching resets the current directory to root.", "drive 1"}, command_drive},
    {{"driver-run", "Loads and starts a PrismCC .pdr module through the cooperative driver runtime. Press Esc to request shutdown.", "driver-run <path.pdr>", "path.pdr: compiled PrismCC driver package with driver_init, driver_poll, driver_shutdown, and main entry points.", "driver-run /DRIVERS/SERIAL.PDR"}, command_driver_run},
    {{"echo", "Prints the command's remaining text to the console. Useful for quick shell messages.", "echo <text>", "text: message to display; spaces are preserved.", "echo Hello PrismOS"}, command_echo},
    {{"edit", "Opens a text file in the built-in full-screen editor. Save or exit using the editor's on-screen controls.", "edit <path>", "path: file path to open or create.", "edit /NOTES.TXT"}, command_edit},
    {{"help", "Opens the interactive command browser, or prints syntax, parameters, and an example for one named command.", "help [command]", "command: optional command name to look up.", "help drive"}, command_help},
    {{"ide", "Opens the integrated development environment for a source file, with editing and PrismCC build controls.", "ide <path>", "path: source file path to open in the IDE.", "ide /HELLO.C"}, command_ide},
    {{"ls", "Lists files and subdirectories in the current directory or in a specified path, including file sizes.", "ls [path]", "path: optional directory path; defaults to the current directory.", "ls /DATA"}, command_ls},
    {{"mkdir", "Creates a new directory at the given path on the active FAT32 volume.", "mkdir <path>", "path: directory path to create; its parent directory must already exist.", "mkdir /DOCS"}, command_mkdir},
    {{"mv", "Moves or renames a file by copying it and then removing the source. Directory moves are not supported; file size is limited by the move buffer.", "mv <source> <destination>", "source: existing file path. destination: new file path; it must not be a directory.", "mv /OLD.TXT /NEW.TXT"}, command_mv},
    {{"partitions", "Opens the interactive disk and partition manager, with text commands available for scripting.", "partitions [ui|list|mount <slot>|create <sizeMiB>|shrink <slot> <reduceByMiB>|delete <slot>|rename <slot> <label>]", "No arguments or ui: open the full-screen manager. list: show a text overview. mount: select a formatted FAT32 volume by 1-based MBR slot; this is different from a physical drive index. create: size in MiB; the new partition is unformatted. shrink: reclaim this many MiB from the free tail of the mounted FAT32 primary partition. delete: 1-based MBR slot; data is not erased. rename: change the mounted FAT32 volume label.", "partitions mount 2"}, command_partitions},
    {{"reboot", "Immediately restarts the computer or emulator. Unsaved filesystem or editor changes may be lost.", "reboot", "None.", "reboot"}, command_reboot},
    {{"rm", "Removes a file from the active FAT32 volume. Use rmdir for an empty directory.", "rm <path>", "path: existing file path; directories are not accepted.", "rm /OLD.TXT"}, command_rm},
    {{"rmdir", "Removes an empty directory. The operation fails if the directory contains entries.", "rmdir <path>", "path: existing empty directory path.", "rmdir /EMPTY"}, command_rmdir},
    {{"shutdown", "Requests a system or emulator power-off. On hardware, platform support may vary.", "shutdown", "None.", "shutdown"}, command_shutdown},
    {{"touch", "Creates an empty file. It fails if the target already exists or its parent directory is missing.", "touch <path>", "path: file path to create.", "touch /NOTES.TXT"}, command_touch},
    {{"write", "Creates or overwrites a file with the supplied text. Existing contents are replaced; no newline is added automatically.", "write <path> <text>", "path: destination file path. text: all remaining text after the path token.", "write /NOTES.TXT Hello"}, command_write},
};

static const int command_count = (int)(sizeof(commands) / sizeof(commands[0]));

static const Command* command_find(const char* name) {
    for (int index = 0; index < command_count; index++) {
        if (string_equals(commands[index].help.name, name)) {
            return &commands[index];
        }
    }

    return 0;
}

static void command_help(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    const char* remainder = 0;

    DEBUG_LOG("help command executed");
    if (parse_token(arguments, token, sizeof(token), &remainder) != 0) {
        (void)help_app_run();
        return;
    }

    if (*remainder != '\0') {
        console_writeln("Usage: help [command]");
        return;
    }

    const Command* command = command_find(token);
    if (command == 0) {
        console_write("No help found for: ");
        console_writeln(token);
        return;
    }

    console_write(command->help.name);
    console_write(" - ");
    console_writeln(command->help.description);
    console_write("Syntax: ");
    console_writeln(command->help.usage);
    console_write("Parameters: ");
    console_writeln(command->help.parameters);
    console_write("Example: ");
    console_writeln(command->help.example);
}

static void command_clear(const char* arguments) {
    (void)arguments;
    DEBUG_LOG("clear command executed");
    console_clear();
}

static void command_echo(const char* arguments) {
    DEBUG_LOG("echo command executed");
    console_writeln(arguments);
}

static void command_about(const char* arguments) {
    (void)arguments;
    DEBUG_LOG("about command executed");
    const uint32_t total_memory_kb = PRISMOS_TOTAL_MEMORY_KB;

    // Keep the version with the general system summary so users only need one command.
    console_writeln("PrismOS version 0.1 beta - early development stage");
    console_writeln("PrismOS is running in a 32-bit shell.");

    if (total_memory_kb == 0) {
        console_writeln("RAM: unknown");
        return;
    }

    // Print the largest readable unit without needing decimals.
    if (total_memory_kb >= 1024U * 1024U) {
        console_write("RAM: ");
        console_write_uint(total_memory_kb / (1024U * 1024U));
        console_writeln(" GB");
        return;
    }

    console_write("RAM: ");
    console_write_uint(total_memory_kb / 1024U);
    console_writeln(" MB");
}

static uint32_t command_ratio_percent(uint32_t numerator, uint32_t denominator) {
    uint32_t remainder = 0U;
    uint32_t percent = 0U;

    if (denominator == 0U) {
        return 0U;
    }
    if (numerator >= denominator) {
        return 100U;
    }

    /* Repeated modular addition avoids numerator * 100 overflowing 32-bit arithmetic. */
    for (uint32_t step = 0; step < 100U; step++) {
        if (remainder >= denominator - numerator) {
            remainder -= denominator - numerator;
            percent++;
        } else {
            remainder += numerator;
        }
    }

    return percent;
}

static void command_print_usage_bar(uint32_t used, uint32_t total) {
    const uint32_t width = 32U;
    uint32_t percent = command_ratio_percent(used, total);
    uint32_t filled = (percent * width) / 100U;

    console_write("[");
    for (uint32_t index = 0; index < width; index++) {
        console_write_char(index < filled ? '#' : '-');
    }
    console_write("] ");
    console_write_uint(percent);
    console_writeln("% used");
}

static void command_print_mib(uint32_t sectors) {
    console_write_uint(sectors / 2048U);
    console_write_char('.');
    console_write_uint(((sectors % 2048U) * 10U) / 2048U);
    console_write(" MiB");
}

static void command_print_partition_overview(void) {
    uint32_t count = partition_manager_count();
    uint32_t drive_sectors = blockdev_device_sector_count();
    uint32_t partition_sectors = 0U;
    uint32_t unallocated_sectors = 0U;
    uint32_t total_fs_sectors = 0U;
    uint32_t used_fs_sectors = 0U;
    uint32_t free_fs_sectors = 0U;
    int superfloppy = partition_manager_is_superfloppy();
    int have_space = vfs_get_space(&total_fs_sectors, &used_fs_sectors, &free_fs_sectors) == 0;

    console_write("Drive ");
    console_write_uint(blockdev_current_drive());
    console_write(" capacity: ");
    command_print_mib(drive_sectors);
    console_write(" (");
    console_write_uint(drive_sectors);
    console_writeln(" sectors)");

    if (superfloppy) {
        console_writeln("Layout: FAT32 superfloppy (no editable MBR entries)");
    } else if (partition_manager_get_usage(&partition_sectors, &unallocated_sectors) == 0) {
        console_write("Partitioned: ");
        command_print_mib(partition_sectors);
        console_write("  Unallocated: ");
        command_print_mib(unallocated_sectors);
        console_writeln("");
    } else {
        console_writeln("Partition-space totals unavailable (invalid MBR layout)");
    }

    if (have_space) {
        char label[12];
        console_write("Mounted FAT32 volume");
        if (vfs_get_volume_label(label, sizeof(label)) == 0 && label[0] != '\0') {
            console_write(" (");
            console_write(label);
            console_write(")");
        }
        console_writeln(":");
        console_write("  Used: ");
        command_print_mib(used_fs_sectors);
        console_write("  Available: ");
        command_print_mib(free_fs_sectors);
        console_write("  Total: ");
        command_print_mib(total_fs_sectors);
        console_write("\n  ");
        command_print_usage_bar(used_fs_sectors, total_fs_sectors);
    } else {
        console_writeln("Mounted FAT32 usage unavailable");
    }

    if (count == 0U) {
        console_writeln("No partition entries detected");
        return;
    }

    console_writeln("Partitions (slot, type, start LBA, size, boot, status):");
    for (uint32_t index = 0; index < count; index++) {
        partition_info_t partition;
        if (partition_manager_get(index, &partition) != 0) {
            continue;
        }

        console_write("  ");
        console_write_uint(index + 1U);
        console_write(": ");
        if (superfloppy) {
            console_write("FAT32 superfloppy");
        } else if (partition.type == 0x0CU) {
            console_write("FAT32-LBA (12)");
        } else if (partition.type == 0x0BU) {
            console_write("FAT32 (11)");
        } else if (partition.type == 0U && partition.start_sector == 0U && partition.sector_count == 0U) {
            console_writeln("empty slot");
            continue;
        } else {
            console_write("type ");
            console_write_uint(partition.type);
        }

        console_write("  start=");
        console_write_uint(partition.start_sector);
        console_write("  size=");
        command_print_mib(partition.sector_count);
        console_write("  boot=");
        console_write(partition.bootable ? "yes" : "no");
        console_write("  ");
        if (!partition.valid) {
            console_writeln("invalid range");
        } else if (partition.is_selected) {
            console_writeln("mounted");
        } else {
            console_writeln("not mounted");
        }
    }
}

static int command_refresh_partition_mount(void) {
    vfs_unmount();
    if (partition_manager_init() != 0 || vfs_init() != 0) {
        return -1;
    }
    return 0;
}

static void command_partitions(const char* arguments) {
    char operation[COMMAND_TOKEN_CAPACITY];
    char first_argument[COMMAND_TOKEN_CAPACITY];
    char second_argument[COMMAND_TOKEN_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, operation, sizeof(operation), &remainder) != 0) {
        (void)partition_manager_app_run();
        return;
    }

    if (string_equals(operation, "ui")) {
        if (*remainder != '\0') {
            console_writeln("Usage: partitions ui");
            return;
        }
        (void)partition_manager_app_run();
        return;
    }

    if (string_equals(operation, "list")) {
        if (*remainder != '\0') {
            console_writeln("Usage: partitions [list]");
            return;
        }
        command_print_partition_overview();
        return;
    }

    if (string_equals(operation, "mount")) {
        uint32_t index;
        uint32_t previous_index = 0U;
        int had_previous = partition_manager_get_selected_index(&previous_index) == 0;
        int status;

        if (parse_token(remainder, first_argument, sizeof(first_argument), &remainder) != 0
            || *remainder != '\0' || parse_drive_index(first_argument, &index) != 0 || index == 0U) {
            console_writeln("Usage: partitions mount <1-based-slot>");
            return;
        }

        vfs_unmount();
        status = partition_manager_select(index);
        if (status == PARTITION_MANAGER_OK && vfs_init() == 0) {
            command_cwd[0] = '/';
            command_cwd[1] = '\0';
            console_write("Mounted FAT32 partition slot ");
            console_write_uint(index);
            console_writeln(" at /");
        } else {
            vfs_unmount();
            if (had_previous) {
                (void)partition_manager_select(previous_index);
                (void)vfs_init();
            } else {
                (void)command_refresh_partition_mount();
            }
            if (status == PARTITION_MANAGER_ERROR_UNSUPPORTED) {
                console_writeln("Partition is not formatted with a supported FAT32 volume; format it first");
            } else if (status == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
                console_writeln("A whole-disk FAT32 volume has no MBR partition slots");
            } else {
                console_writeln("Could not mount that partition; previous volume restored");
            }
        }
        return;
    }

    if (string_equals(operation, "create")) {
        uint32_t size_mib;
        uint32_t slot;
        uint32_t start_sector;
        int status;

        if (parse_token(remainder, first_argument, sizeof(first_argument), &remainder) != 0
            || *remainder != '\0'
            || parse_drive_index(first_argument, &size_mib) != 0
            || size_mib == 0U) {
            console_writeln("Usage: partitions create <sizeMiB> (size must be a positive integer)");
            return;
        }

        status = partition_manager_create(size_mib, &slot, &start_sector);
        if (status == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
            console_writeln("Cannot create MBR partitions on a FAT32 superfloppy disk");
        } else if (status == PARTITION_MANAGER_ERROR_NO_SLOT) {
            console_writeln("Cannot create partition: all four primary MBR slots are occupied");
        } else if (status == PARTITION_MANAGER_ERROR_NO_SPACE) {
            console_writeln("Cannot create partition: insufficient contiguous unallocated space");
        } else if (status == PARTITION_MANAGER_ERROR_ARGUMENT) {
            console_writeln("Invalid partition size; enter a positive MiB value within drive limits");
        } else if (status == PARTITION_MANAGER_ERROR_TABLE) {
            console_writeln("Cannot create partition: MBR is invalid, overlapping, or unsupported");
        } else if (status != PARTITION_MANAGER_OK) {
            console_writeln("Cannot create partition: disk read/write failed");
        } else {
            console_write("Created FAT32-type partition in MBR slot ");
            console_write_uint(slot);
            console_write(" at LBA ");
            console_write_uint(start_sector);
            console_write(" (");
            console_write_uint(size_mib);
            console_writeln(" MiB). It is unformatted; format it before mounting.");
            if (command_refresh_partition_mount() != 0) {
                console_writeln("Warning: partition table changed, but the mounted FAT32 volume could not be restored");
            }
            command_print_partition_overview();
        }
        return;
    }

    if (string_equals(operation, "delete")) {
        uint32_t index;
        int status;

        if (parse_token(remainder, first_argument, sizeof(first_argument), &remainder) != 0
            || *remainder != '\0' || parse_drive_index(first_argument, &index) != 0 || index == 0U) {
            console_writeln("Usage: partitions delete <1-based-slot>");
            return;
        }

        status = partition_manager_delete(index);
        if (status == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
            console_writeln("Cannot delete MBR entries on a FAT32 superfloppy disk");
        } else if (status == PARTITION_MANAGER_ERROR_PROTECTED) {
            console_writeln("Cannot delete the mounted partition; switch to another volume first");
        } else if (status == PARTITION_MANAGER_ERROR_NO_SLOT) {
            console_writeln("Partition slot is empty or does not exist");
        } else if (status == PARTITION_MANAGER_ERROR_ARGUMENT) {
            console_writeln("Partition slot must be between 1 and 4");
        } else if (status == PARTITION_MANAGER_ERROR_UNSUPPORTED) {
            console_writeln("Cannot delete extended or protective entries; logical/GPT partitions are unsupported");
        } else if (status == PARTITION_MANAGER_ERROR_TABLE) {
            console_writeln("Cannot delete partition: table entry is invalid or changed");
        } else if (status != PARTITION_MANAGER_OK) {
            console_writeln("Cannot delete partition: disk read/write failed");
        } else {
            console_writeln("Partition entry deleted. Data in its former range was not erased.");
            if (command_refresh_partition_mount() != 0) {
                console_writeln("Warning: partition table changed, but the mounted FAT32 volume could not be restored");
            }
            command_print_partition_overview();
        }
        return;
    }

    if (string_equals(operation, "shrink")) {
        uint32_t index;
        uint32_t shrink_mib;
        int status;

        if (parse_token(remainder, first_argument, sizeof(first_argument), &remainder) != 0
            || parse_token(remainder, second_argument, sizeof(second_argument), &remainder) != 0
            || *remainder != '\0' || parse_drive_index(first_argument, &index) != 0 || index == 0U
            || parse_drive_index(second_argument, &shrink_mib) != 0 || shrink_mib == 0U) {
            console_writeln("Usage: partitions shrink <mounted-slot> <reduceByMiB>");
            return;
        }

        status = partition_manager_shrink(index, shrink_mib);
        if (status == PARTITION_MANAGER_ERROR_DATA_PRESENT) {
            console_writeln("Cannot shrink: allocated data still occupies the partition tail");
        } else if (status == PARTITION_MANAGER_ERROR_PROTECTED) {
            console_writeln("Only the mounted FAT32 primary partition can be shrunk");
        } else if (status == PARTITION_MANAGER_ERROR_ARGUMENT) {
            console_writeln("Invalid shrink size; it must leave a valid FAT32 volume");
        } else if (status == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
            console_writeln("Cannot shrink a FAT32 superfloppy as an MBR partition");
        } else if (status == PARTITION_MANAGER_ERROR_TABLE) {
            console_writeln("Cannot shrink an invalid or unsupported MBR layout");
        } else if (status == PARTITION_MANAGER_ERROR_PARTIAL) {
            console_writeln("FAT32 is safely smaller, but the MBR update failed; retry the same shrink amount");
        } else if (status != PARTITION_MANAGER_OK) {
            console_writeln("Cannot shrink partition: disk read/write failed");
        } else {
            console_writeln("Mounted partition shrunk; reclaimed tail space can now hold a new partition");
            if (command_refresh_partition_mount() != 0) {
                console_writeln("Warning: partition size changed, but the FAT32 volume could not be remounted");
            }
            command_print_partition_overview();
        }
        return;
    }

    if (string_equals(operation, "rename")) {
        uint32_t index;
        uint32_t selected_index;

        if (parse_token(remainder, first_argument, sizeof(first_argument), &remainder) != 0
            || parse_token(remainder, second_argument, sizeof(second_argument), &remainder) != 0
            || *remainder != '\0' || parse_drive_index(first_argument, &index) != 0 || index == 0U) {
            console_writeln("Usage: partitions rename <mounted-slot> <label>");
            return;
        }

        if (partition_manager_get_selected_index(&selected_index) != 0 || selected_index != index) {
            console_writeln("Only the currently mounted FAT32 partition can be renamed");
            return;
        }
        if (vfs_set_volume_label(second_argument) != 0) {
            console_writeln("Invalid label or unable to write it; use 1-11 letters, digits, '_' or '-'");
            return;
        }

        console_write("Renamed mounted FAT32 volume to ");
        console_writeln(second_argument);
        command_print_partition_overview();
        return;
    }

    console_writeln("Usage: partitions [ui|list|mount <slot>|create <sizeMiB>|shrink <slot> <reduceByMiB>|delete <slot>|rename <slot> <label>]");
}

static int parse_drive_index(const char* text, uint32_t* out_index) {
    uint32_t value = 0;

    if (*text == '\0') {
        return -1;
    }

    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return -1;
        }

        if (value > (0xFFFFFFFFU - (uint32_t)(*text - '0')) / 10U) {
            return -1;
        }

        value = value * 10U + (uint32_t)(*text - '0');
        text++;
    }

    *out_index = value;
    return 0;
}

static void command_drive(const char* arguments) {
    const char* input = skip_spaces(arguments);
    char token[COMMAND_TOKEN_CAPACITY];
    const char* remainder = 0;

    if (*input == '\0') {
        uint32_t count = blockdev_drive_count();
        if (count == 0U) {
            console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            console_writeln("ATA DRIVE OVERVIEW");
            console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
            console_writeln("No ATA hard drives detected");
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            return;
        }

        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_writeln("ATA DRIVE OVERVIEW");
        console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
        console_writeln("  ID   CAPACITY        STATUS");
        for (uint32_t index = 0; index < count; index++) {
            blockdev_drive_info_t info;
            if (blockdev_get_drive_info(index, &info) != 0) {
                continue;
            }

            int is_current = index == blockdev_current_drive();
            console_set_color(is_current ? COLOR_LIGHT_GREEN : COLOR_LIGHT_GRAY, COLOR_BLACK);
            console_write(is_current ? "  *  " : "     ");
            console_write_uint(index);
            console_write("   ");
            command_print_mib(info.sector_count);
            console_write("   ");
            console_writeln(is_current ? "MOUNTED" : "DETECTED");
        }

        char volume_label[12];
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_write("Mounted volume: ");
        if (vfs_get_volume_label(volume_label, sizeof(volume_label)) == 0 && volume_label[0] != '\0') {
            console_writeln(volume_label);
        } else {
            console_writeln("unavailable");
        }
        console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
        console_writeln("Drive index selects a physical disk; partition slots are separate.");
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_writeln("Switch disk: drive <index>    Mount partition: partitions mount <slot>");
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        return;
    }

    if (parse_token(input, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: drive [index]");
        return;
    }

    uint32_t selected_drive;
    if (parse_drive_index(token, &selected_drive) != 0 ||
        selected_drive >= blockdev_drive_count()) {
        console_writeln("Physical drive index not found; use partitions mount <slot> for an MBR partition");
        return;
    }

    uint32_t previous_drive = blockdev_current_drive();
    vfs_unmount();
    if (blockdev_select_drive(selected_drive) != 0
        || partition_manager_init() != 0 || vfs_init() != 0) {
        vfs_unmount();
        if (blockdev_select_drive(previous_drive) == 0
            && partition_manager_init() == 0) {
            (void)vfs_init();
        }
        console_write("Drive ");
        console_write_uint(selected_drive);
        console_writeln(" has no mountable FAT32 volume; previous drive restored");
        return;
    }

    command_cwd[0] = '/';
    command_cwd[1] = '\0';
    console_write("Switched to drive ");
    console_write_uint(selected_drive);
    console_writeln(" and mounted its FAT32 volume at /");
}

static void command_reboot(const char* arguments) {
    (void)arguments;
    WARNINIG_LOG("reboot command executed");
    console_writeln("Rebooting...");
    reboot_system();
}

static void command_shutdown(const char* arguments) {
    (void)arguments;
    WARNINIG_LOG("shutdown command executed");
    console_writeln("Shutting down...");
    shutdown_system();
}

static void command_comport(const char* arguments) {
    if (*arguments == '\0') {
        WARNINIG_LOG("comport command missing arguments");
        console_writeln("Usage: comport <text>");
        return;
    }

    if (comport_init() != 0) {
        ERROR_LOG("COM1 initialization failed in comport command");
        console_writeln("COM1 init failed");
        return;
    }

    // Forward user-provided payload directly to COM1 for live diagnostics.
    DEBUG_LOG("comport command writing data to COM1");
    comport_write_string(arguments);
    comport_write_char('\n');

    console_write("Sent ");
    console_write_uint(string_length(arguments));
    console_writeln(" bytes to COM1");
}

static void command_ls(const char* arguments) {
    char target[COMMAND_PATH_CAPACITY];
    const char* path = skip_spaces(arguments);
    LsCommandContext context = {0};

    if (*path == '\0') {
        path = command_cwd;
    }

    if (resolve_to_absolute_path(path, target, sizeof(target)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_list(target, command_ls_visit, &context) != 0) {
        ERROR_LOG("ls failed to read directory");
        console_writeln("ls failed");
        return;
    }

    if (context.count == 0) {
        console_writeln("(empty)");
    }
}

static void command_cd(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;
    int is_dir = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: cd <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_path_is_dir(absolute, &is_dir) != 0 || !is_dir) {
        console_writeln("Directory not found");
        return;
    }

    copy_string_limited(command_cwd, absolute, sizeof(command_cwd));
}

static void command_touch(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: touch <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0 || vfs_touch(absolute) != 0) {
        console_writeln("touch failed");
        return;
    }
}

static void command_mkdir(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: mkdir <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0 || vfs_mkdir(absolute) != 0) {
        console_writeln("mkdir failed");
        return;
    }
}

static void command_rm(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: rm <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0 || vfs_rm(absolute) != 0) {
        console_writeln("rm failed");
    }
}

static void command_rmdir(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: rmdir <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0 || vfs_rmdir(absolute) != 0) {
        console_writeln("rmdir failed (directory must be empty)");
    }
}

static void command_delete(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;
    int is_dir = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: delete <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_path_is_dir(absolute, &is_dir) != 0) {
        console_writeln("Path not found");
        return;
    }

    if (is_dir) {
        if (vfs_rmdir(absolute) != 0) {
            console_writeln("delete failed (directory must be empty)");
        }
    } else if (vfs_rm(absolute) != 0) {
        console_writeln("delete failed");
    }
}

static void command_cat(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    char content[COMMAND_TEXT_CAPACITY + 1];
    uint32_t size = 0;
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: cat <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_read_file(absolute, content, sizeof(content), &size) != 0) {
        console_writeln("cat failed");
        return;
    }

    if (size == 0U) {
        console_writeln("(empty)");
        return;
    }

    console_write(content);
    if (content[size - 1U] != '\n') {
        console_write_char('\n');
    }
}

static void command_write(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* text = 0;

    if (parse_token(arguments, token, sizeof(token), &text) != 0 || *text == '\0') {
        console_writeln("Usage: write <path> <text>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_write_file(absolute, text, (uint32_t)string_length(text), 0) != 0) {
        console_writeln("write failed");
    }
}

static void command_append(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* text = 0;

    if (parse_token(arguments, token, sizeof(token), &text) != 0 || *text == '\0') {
        console_writeln("Usage: append <path> <text>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (vfs_write_file(absolute, text, (uint32_t)string_length(text), 1) != 0) {
        console_writeln("append failed");
    }
}

static void command_mv(const char* arguments) {
    char src_token[COMMAND_TOKEN_CAPACITY];
    char dst_token[COMMAND_TOKEN_CAPACITY];
    char src_absolute[COMMAND_PATH_CAPACITY];
    char dst_absolute[COMMAND_PATH_CAPACITY];
    char content[COMMAND_MOVE_BUFFER_CAPACITY + 1U];
    uint32_t size = 0;
    const char* remainder = 0;
    int src_is_dir = 0;
    int dst_is_dir = 0;

    if (parse_token(arguments, src_token, sizeof(src_token), &remainder) != 0) {
        console_writeln("Usage: mv <source> <destination>");
        return;
    }

    if (parse_token(remainder, dst_token, sizeof(dst_token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: mv <source> <destination>");
        return;
    }

    if (resolve_to_absolute_path(src_token, src_absolute, sizeof(src_absolute)) != 0
        || resolve_to_absolute_path(dst_token, dst_absolute, sizeof(dst_absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (string_equals(src_absolute, dst_absolute)) {
        console_writeln("mv skipped: source and destination are the same");
        return;
    }

    if (vfs_path_is_dir(src_absolute, &src_is_dir) != 0) {
        console_writeln("mv failed: source path not found");
        return;
    }

    if (src_is_dir) {
        console_writeln("mv failed: directory move not supported yet");
        return;
    }

    if (vfs_path_is_dir(dst_absolute, &dst_is_dir) == 0 && dst_is_dir) {
        console_writeln("mv failed: destination is a directory");
        return;
    }

    if (vfs_read_file(src_absolute, content, sizeof(content), &size) != 0) {
        console_writeln("mv failed: cannot read source file (file may be too large)");
        return;
    }

    if (vfs_write_file(dst_absolute, content, size, 0) != 0) {
        console_writeln("mv failed: cannot write destination file");
        return;
    }

    if (vfs_rm(src_absolute) != 0) {
        console_writeln("mv warning: copied but failed to remove source");
        return;
    }
}

static void command_edit(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: edit <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (app_manager_run_editor(absolute) != 0) {
        console_writeln("editor failed");
    }
}

static void command_ide(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: ide <path>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (app_manager_run_ide(absolute) != 0) {
        console_writeln("ide failed");
    }
}

static void command_app_run(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0) {
        console_writeln("Usage: app-run <path> [args]");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid path");
        return;
    }

    if (app_manager_run_path(absolute, remainder) != 0) {
        console_writeln("app execution failed");
    }
}

static void command_driver_run(const char* arguments) {
    char token[COMMAND_TOKEN_CAPACITY];
    char absolute[COMMAND_PATH_CAPACITY];
    const char* remainder = 0;

    if (parse_token(arguments, token, sizeof(token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: driver-run <path.pdr>");
        return;
    }

    if (resolve_to_absolute_path(token, absolute, sizeof(absolute)) != 0) {
        console_writeln("Invalid driver path");
        return;
    }

    if (app_manager_run_driver_path(absolute) != 0) {
        console_writeln("driver load or execution failed");
    }
}

static void command_cc(const char* arguments) {
    char input_token[COMMAND_TOKEN_CAPACITY];
    char output_token[COMMAND_TOKEN_CAPACITY];
    char input_absolute[COMMAND_PATH_CAPACITY];
    char output_absolute[COMMAND_PATH_CAPACITY];
    char error[96];
    const char* remainder = 0;

    if (parse_token(arguments, input_token, sizeof(input_token), &remainder) != 0) {
        console_writeln("Usage: cc <input.c> <output.app|output.pdr>");
        return;
    }

    if (parse_token(remainder, output_token, sizeof(output_token), &remainder) != 0 || *remainder != '\0') {
        console_writeln("Usage: cc <input.c> <output.app|output.pdr>");
        return;
    }

    if (resolve_to_absolute_path(input_token, input_absolute, sizeof(input_absolute)) != 0) {
        console_writeln("Invalid input path");
        return;
    }

    if (resolve_to_absolute_path(output_token, output_absolute, sizeof(output_absolute)) != 0) {
        console_writeln("Invalid output path");
        return;
    }

    if (prismcc_compile_file(input_absolute, output_absolute, error, sizeof(error)) != 0) {
        console_write("cc failed: ");
        console_writeln(error[0] == '\0' ? "compile error" : error);
        return;
    }

    console_write("Compiled package: ");
    console_writeln(output_absolute);
}

const char* command_get_cwd(void) {
    return command_cwd;
}

uint32_t command_get_help_count(void) {
    return (uint32_t)command_count;
}

const command_help_entry_t* command_get_help_entry(uint32_t index) {
    if (index >= (uint32_t)command_count) {
        return 0;
    }

    return &commands[index].help;
}

void command_print_help(void) {
    console_writeln("PrismOS commands (use help to open the command browser):");

    for (int index = 0; index < command_count; index++) {
        console_write("  ");
        console_write(commands[index].help.name);
        console_write(" - ");
        console_writeln(commands[index].help.description);
    }
}

void command_execute(const char* line) {
    char command_name[16];
    const char* arguments;
    int index = 0;

    line = skip_spaces(line);
    if (*line == '\0') {
        DEBUG_LOG("empty command ignored");
        return;
    }

    while (line[index] != '\0' && line[index] != ' ' && index < (int)(sizeof(command_name) - 1)) {
        command_name[index] = line[index];
        index++;
    }

    command_name[index] = '\0';
    arguments = skip_spaces(line + index);

    const Command* command = command_find(command_name);
    if (command != 0) {
        // Emit one trace per successful command before handing over control.
        DEBUG_LOG("command resolved and dispatched");
        command->handler(arguments);
        return;
    }

    WARNINIG_LOG("unknown command entered");
    console_write("Unknown command: ");
    console_writeln(command_name);
}