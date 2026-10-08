#include "apps/partition_manager_app.h"

#include <stdint.h>

#include "display/console.h"
#include "filesystem/blockdev.h"
#include "filesystem/fat32/fat32.h"
#include "filesystem/partition_manager.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"

#define UI_MIN_COLUMNS 76U
#define UI_MIN_ROWS 19U
#define UI_INPUT_CAPACITY 12U
#define UI_TABLE_HEADER_ROW 8U
#define UI_TABLE_FIRST_ROW 9U

typedef enum {
    UI_MODAL_NONE = 0,
    UI_MODAL_CREATE,
    UI_MODAL_SHRINK,
    UI_MODAL_DELETE,
    UI_MODAL_RENAME,
    UI_MODAL_FORMAT,
} UiModal;

typedef struct {
    uint32_t columns;
    uint32_t rows;
    uint32_t selected_slot;
    uint32_t input_length;
    char input[UI_INPUT_CAPACITY];
    char status[96];
    VGA_Color status_color;
    UiModal modal;
    int running;
} PartitionUiState;

static void ui_copy(char* destination, uint32_t capacity, const char* source) {
    uint32_t index = 0U;
    if (capacity == 0U) {
        return;
    }
    while (source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static void ui_status(PartitionUiState* state, const char* message, VGA_Color color) {
    ui_copy(state->status, sizeof(state->status), message);
    state->status_color = color;
}

static void ui_write_clipped(const char* text, uint32_t width) {
    for (uint32_t index = 0U; text[index] != '\0' && index < width; index++) {
        console_write_char(text[index]);
    }
}

static void ui_fill_row(uint32_t row, VGA_Color foreground, VGA_Color background) {
    console_set_color(foreground, background);
    console_clear_row((int)row);
}

static void ui_row(uint32_t row, VGA_Color foreground, VGA_Color background, const char* text) {
    ui_fill_row(row, foreground, background);
    console_set_cursor(0, (int)row);
    ui_write_clipped(text, console_get_framebuffer_width() / 8U);
}

static void ui_clear_row(uint32_t row) {
    ui_fill_row(row, COLOR_LIGHT_GRAY, COLOR_BLACK);
}

static void ui_write_number(uint32_t value) {
    console_write_uint(value);
}

static void ui_write_mib(uint32_t sectors) {
    ui_write_number(sectors / 2048U);
    console_write_char('.');
    ui_write_number(((sectors % 2048U) * 10U) / 2048U);
    console_write(" MiB");
}

static uint32_t ui_percent(uint32_t numerator, uint32_t denominator) {
    uint32_t remainder = 0U;
    uint32_t percent = 0U;

    if (denominator == 0U) {
        return 0U;
    }
    if (numerator >= denominator) {
        return 100U;
    }

    for (uint32_t index = 0U; index < 100U; index++) {
        if (remainder >= denominator - numerator) {
            remainder -= denominator - numerator;
            percent++;
        } else {
            remainder += numerator;
        }
    }
    return percent;
}

static void ui_draw_bar(uint32_t row, const char* label, uint32_t used, uint32_t total, const char* suffix) {
    uint32_t percent = ui_percent(used, total);
    uint32_t fill = (percent * 24U) / 100U;
    uint32_t x = 2U;

    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_set_cursor((int)x, (int)row);
    console_write(label);
    x += 13U;
    console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    console_set_cursor((int)x++, (int)row);
    console_write_char('[');
    for (uint32_t index = 0U; index < 24U; index++) {
        console_set_color(index < fill ? COLOR_LIGHT_GREEN : COLOR_DARK_GRAY, COLOR_BLACK);
        console_set_cursor((int)x++, (int)row);
        console_write_char(index < fill ? '#' : '-');
    }
    console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    console_set_cursor((int)x++, (int)row);
    console_write_char(']');
    console_set_cursor((int)x++, (int)row);
    console_write_char(' ');
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_set_cursor((int)x, (int)row);
    ui_write_number(percent);
    console_write(suffix);
}

static const char* ui_partition_type(const partition_info_t* partition) {
    if (partition_manager_is_superfloppy()) {
        return "FAT32 disk";
    }
    if (partition->type == 0x0CU) {
        return "FAT32-LBA";
    }
    if (partition->type == 0x0BU) {
        return "FAT32";
    }
    if (partition->type == 0x05U || partition->type == 0x0FU || partition->type == 0x85U) {
        return "Extended";
    }
    if (partition->type == 0xEEU) {
        return "GPT protect";
    }
    return "Other";
}

static void ui_draw_partition_row(const PartitionUiState* state, uint32_t slot) {
    partition_info_t partition;
    uint32_t row = UI_TABLE_FIRST_ROW + slot;
    uint32_t count = partition_manager_count();
    int have_partition = slot < count && partition_manager_get(slot, &partition) == 0;
    int empty = !have_partition || (partition.type == 0U && partition.start_sector == 0U
        && partition.sector_count == 0U);
    int selected = state->selected_slot == slot;
    VGA_Color foreground = selected ? COLOR_WHITE : COLOR_LIGHT_GRAY;
    VGA_Color background = selected ? COLOR_BLUE : COLOR_BLACK;

    console_set_color(foreground, background);
    console_clear_row((int)row);
    console_set_color(foreground, background);
    console_set_cursor(1, (int)row);
    console_write(selected ? "> " : "  ");
    console_write_char((char)('1' + slot));
    console_set_cursor(8, (int)row);

    if (empty) {
        console_set_color(selected ? COLOR_WHITE : COLOR_DARK_GRAY, background);
        console_write("Empty slot");
        return;
    }

    console_write(ui_partition_type(&partition));
    if (partition.type != 0x0CU && partition.type != 0x0BU
        && partition.type != 0x05U && partition.type != 0x0FU
        && partition.type != 0x85U && partition.type != 0xEEU) {
        console_write(" ");
        ui_write_number(partition.type);
    }
    console_set_cursor(22, (int)row);
    ui_write_number(partition.start_sector);
    console_set_cursor(37, (int)row);
    ui_write_mib(partition.sector_count);
    console_set_cursor(53, (int)row);
    if (partition.is_selected) {
        console_set_color(COLOR_LIGHT_GREEN, background);
        console_write("MOUNTED");
    } else if (!partition.valid) {
        console_set_color(COLOR_LIGHT_RED, background);
        console_write("INVALID");
    } else {
        console_write("UNMOUNTED");
    }
    console_set_cursor(69, (int)row);
    console_write(partition.bootable ? "Boot" : " ");
}

static void ui_draw_modal(const PartitionUiState* state) {
    uint32_t box_width = state->columns > 68U ? 64U : state->columns - 4U;
    uint32_t box_height = 8U;
    uint32_t x = (state->columns - box_width) / 2U;
    uint32_t y = (state->rows - box_height) / 2U;
    const char* title = "Partition action";
    const char* first_line = "";
    const char* second_line = "";
    const char* input_title = "";
    char prompt[UI_INPUT_CAPACITY + 24U];

    if (state->modal == UI_MODAL_CREATE) {
        title = "CREATE PRIMARY PARTITION";
        first_line = "Choose a positive size. The new partition is not formatted.";
        input_title = "Size in MiB: ";
    } else if (state->modal == UI_MODAL_SHRINK) {
        title = "SHRINK MOUNTED PARTITION";
        first_line = "Reclaims space at the end when no allocated data is there.";
        input_title = "Reduce by MiB: ";
    } else if (state->modal == UI_MODAL_DELETE) {
        title = "DELETE PARTITION ENTRY?";
        first_line = "This removes the MBR entry only; it does not erase data.";
        second_line = "Press Y to confirm, or any other key to cancel.";
    } else if (state->modal == UI_MODAL_RENAME) {
        title = "RENAME FAT32 VOLUME";
        first_line = "Changes the label of the mounted volume (1-11 chars).";
        input_title = "New label: ";
    } else if (state->modal == UI_MODAL_FORMAT) {
        title = "FORMAT FAT32 PARTITION?";
        first_line = "ALL DATA ON THIS PARTITION WILL BE ERASED.";
        second_line = "Press Y to format; any other key cancels.";
    }

    for (uint32_t line = 0U; line < box_height; line++) {
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLUE);
        console_set_cursor((int)x, (int)(y + line));
        for (uint32_t column = 0U; column < box_width; column++) {
            console_write_char(' ');
        }
    }

    console_set_color(COLOR_WHITE, COLOR_BLUE);
    console_set_cursor((int)(x + 2U), (int)(y + 1U));
    ui_write_clipped(title, box_width - 4U);
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLUE);
    console_set_cursor((int)(x + 2U), (int)(y + 3U));
    ui_write_clipped(first_line, box_width - 4U);
    console_set_cursor((int)(x + 2U), (int)(y + 4U));
    ui_write_clipped(second_line, box_width - 4U);

    if (state->modal == UI_MODAL_FORMAT) {
        partition_info_t partition;
        console_set_color(COLOR_YELLOW, COLOR_BLUE);
        console_set_cursor((int)(x + 2U), (int)(y + 5U));
        console_write("Slot ");
        ui_write_number(state->selected_slot + 1U);
        console_write("  |  ");
        if (partition_manager_get(state->selected_slot, &partition) == 0) {
            ui_write_mib(partition.sector_count);
        } else {
            console_write("partition unavailable");
        }
    }

    if (input_title[0] != '\0') {
        ui_copy(prompt, sizeof(prompt), input_title);
        uint32_t length = 0U;
        while (prompt[length] != '\0') {
            length++;
        }
        for (uint32_t index = 0U; index < state->input_length && length + 1U < sizeof(prompt); index++) {
            prompt[length++] = state->input[index];
        }
        prompt[length++] = '_';
        prompt[length] = '\0';
        console_set_color(COLOR_YELLOW, COLOR_BLUE);
        console_set_cursor((int)(x + 2U), (int)(y + 5U));
        ui_write_clipped(prompt, box_width - 4U);
    }

    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLUE);
    console_set_cursor((int)(x + 2U), (int)(y + 6U));
    if (state->modal == UI_MODAL_DELETE) {
        ui_write_clipped("Esc cancels", box_width - 4U);
    } else if (state->modal == UI_MODAL_FORMAT) {
        ui_write_clipped("Y confirms    Esc cancels", box_width - 4U);
    } else {
        ui_write_clipped("Enter accepts    Esc cancels", box_width - 4U);
    }
}

static void ui_draw(const PartitionUiState* state) {
    blockdev_drive_info_t drive;
    uint32_t drive_sectors = blockdev_device_sector_count();
    uint32_t partition_sectors = 0U;
    uint32_t unallocated_sectors = 0U;
    uint32_t total_fs_sectors = 0U;
    uint32_t used_fs_sectors = 0U;
    uint32_t free_fs_sectors = 0U;
    uint32_t selected_drive = blockdev_current_drive();
    int superfloppy = partition_manager_is_superfloppy();
    int have_drive = blockdev_get_drive_info(selected_drive, &drive) == 0;
    int have_partition_usage = partition_manager_get_usage(&partition_sectors, &unallocated_sectors) == 0;
    int have_fs_usage = vfs_get_space(&total_fs_sectors, &used_fs_sectors, &free_fs_sectors) == 0;
    char label[12];

    console_set_cursor_visible(0);
    ui_row(0U, COLOR_WHITE, COLOR_BLUE, " PrismOS     STORAGE & PARTITION MANAGER");
    ui_row(1U, COLOR_LIGHT_CYAN, COLOR_BLACK, " DISK OVERVIEW   |   Review space, inspect partitions, and choose an action");
    ui_clear_row(2U);
    if (have_fs_usage) {
        ui_draw_bar(2U, "Volume used", used_fs_sectors, total_fs_sectors, "% used");
    } else {
        ui_row(2U, COLOR_DARK_GRAY, COLOR_BLACK, " Mounted filesystem usage is unavailable");
    }

    ui_clear_row(3U);
    console_set_color(COLOR_YELLOW, COLOR_BLACK);
    console_set_cursor(2, 3);
    console_write("CURRENT DRIVE");
    ui_clear_row(4U);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_set_cursor(2, 4);
    console_write("Drive ");
    ui_write_number(selected_drive);
    console_write("  |  ");
    if (have_drive) {
        ui_write_mib(drive.sector_count);
        console_write("  |  ");
        ui_write_number(drive.sector_count);
        console_write(" sectors");
    } else {
        console_write("Capacity unavailable");
    }

    ui_clear_row(5U);
    if (superfloppy) {
        console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        console_set_cursor(2, 5);
        console_write("Whole-disk FAT32 volume (superfloppy): MBR editing is disabled");
    } else if (have_partition_usage) {
        ui_draw_bar(5U, "Disk alloc", partition_sectors, drive_sectors, "% allocated");
    } else {
        console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        console_set_cursor(2, 5);
        console_write("Partition totals unavailable: invalid or unsupported MBR layout");
    }

    ui_clear_row(6U);
    if (have_partition_usage && !superfloppy) {
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_set_cursor(2, 6);
        console_write("Partitioned: ");
        ui_write_mib(partition_sectors);
        console_write("    Unallocated: ");
        ui_write_mib(unallocated_sectors);
    }

    ui_clear_row(7U);
    console_set_color(COLOR_YELLOW, COLOR_BLACK);
    console_set_cursor(2, 7);
    console_write("MOUNTED VOLUME");
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_set_cursor(19, 7);
    if (have_fs_usage) {
        if (vfs_get_volume_label(label, sizeof(label)) == 0 && label[0] != '\0') {
            console_write(label);
            console_write("  |  ");
        }
        console_write("Used ");
        ui_write_mib(used_fs_sectors);
        console_write("  Free ");
        ui_write_mib(free_fs_sectors);
        console_write("  Total ");
        ui_write_mib(total_fs_sectors);
    } else {
        console_write("No mounted FAT32 volume");
    }

    ui_row(UI_TABLE_HEADER_ROW, COLOR_WHITE, COLOR_DARK_GRAY,
        "   SLOT  TYPE             START LBA      SIZE             STATUS       FLAGS");
    for (uint32_t slot = 0U; slot < PARTITION_MANAGER_MAX_PARTITIONS; slot++) {
        ui_draw_partition_row(state, slot);
    }
    ui_clear_row(13U);

    ui_row(14U, COLOR_LIGHT_CYAN, COLOR_BLACK,
        " C Create    S Shrink mounted    D Delete    F Format    M Mount");
    ui_row(15U, COLOR_LIGHT_GRAY, COLOR_BLACK,
        " R Rename label    Up/Down select    Switch disks: drive <index>");
    ui_row(state->rows - 3U, state->status_color, COLOR_BLACK, state->status);
    ui_row(state->rows - 2U, COLOR_LIGHT_GRAY, COLOR_BLACK,
        "Changes write to disk. New partitions need formatting before they can be mounted.");
    ui_row(state->rows - 1U, COLOR_BLACK, COLOR_LIGHT_GRAY,
        " Esc / Q  Exit                                      PrismOS Disk Manager ");
    for (uint32_t row = 16U; row + 3U < state->rows; row++) {
        ui_clear_row(row);
    }

    if (state->modal != UI_MODAL_NONE) {
        ui_draw_modal(state);
    }
}

static void ui_open_modal(PartitionUiState* state, UiModal modal) {
    state->modal = modal;
    state->input_length = 0U;
    state->input[0] = '\0';
}

static int ui_parse_size(const char* text, uint32_t* out_size) {
    uint32_t value = 0U;
    if (text[0] == '\0') {
        return -1;
    }
    for (uint32_t index = 0U; text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return -1;
        }
        uint32_t digit = (uint32_t)(text[index] - '0');
        if (value > (0xFFFFFFFFU - digit) / 10U) {
            return -1;
        }
        value = value * 10U + digit;
    }
    if (value == 0U) {
        return -1;
    }
    *out_size = value;
    return 0;
}

static int ui_refresh_volume(void) {
    vfs_unmount();
    if (partition_manager_init() != 0) {
        return -1;
    }
    return vfs_init();
}

static void ui_create_partition(PartitionUiState* state) {
    uint32_t size_mib;
    uint32_t slot;
    uint32_t start_sector;
    int result;

    if (ui_parse_size(state->input, &size_mib) != 0) {
        ui_status(state, "Enter a positive whole-number size in MiB.", COLOR_LIGHT_RED);
        return;
    }
    result = partition_manager_create(size_mib, &slot, &start_sector);
    if (result == PARTITION_MANAGER_OK) {
        state->selected_slot = slot - 1U;
        if (ui_refresh_volume() == 0) {
            ui_status(state, "Partition created. It is unformatted; format it before mounting.", COLOR_LIGHT_GREEN);
        } else {
            ui_status(state, "MBR updated, but the mounted FAT32 volume could not be restored.", COLOR_LIGHT_RED);
        }
    } else if (result == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
        ui_status(state, "Cannot create MBR entries on a FAT32 superfloppy disk.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_NO_SLOT) {
        ui_status(state, "All four primary MBR slots are occupied.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_NO_SPACE) {
        ui_status(state, "Not enough contiguous unallocated space for that size.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_ARGUMENT) {
        ui_status(state, "Size exceeds the supported drive limits.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_TABLE) {
        ui_status(state, "Cannot edit an invalid, overlapping, or unsupported MBR layout.", COLOR_LIGHT_RED);
    } else {
        ui_status(state, "Disk read/write failed while creating the partition.", COLOR_LIGHT_RED);
    }
}

static void ui_delete_partition(PartitionUiState* state) {
    int result = partition_manager_delete(state->selected_slot + 1U);
    if (result == PARTITION_MANAGER_OK) {
        if (ui_refresh_volume() == 0) {
            ui_status(state, "Partition entry removed. Data in its former range was not erased.", COLOR_LIGHT_GREEN);
        } else {
            ui_status(state, "MBR updated, but the mounted FAT32 volume could not be restored.", COLOR_LIGHT_RED);
        }
    } else if (result == PARTITION_MANAGER_ERROR_PROTECTED) {
        ui_status(state, "The mounted partition is protected and cannot be deleted.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_UNSUPPORTED) {
        ui_status(state, "Extended and GPT-protective entries are not supported for deletion.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_NO_SLOT) {
        ui_status(state, "Select an occupied partition slot first.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
        ui_status(state, "This whole-disk FAT32 volume has no editable MBR entries.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_TABLE) {
        ui_status(state, "The selected MBR entry is invalid or has changed.", COLOR_LIGHT_RED);
    } else {
        ui_status(state, "Disk read/write failed while deleting the entry.", COLOR_LIGHT_RED);
    }
}

static void ui_shrink_partition(PartitionUiState* state) {
    uint32_t shrink_mib;
    int result;

    if (ui_parse_size(state->input, &shrink_mib) != 0) {
        ui_status(state, "Enter a positive whole-number shrink amount in MiB.", COLOR_LIGHT_RED);
        return;
    }
    result = partition_manager_shrink(state->selected_slot + 1U, shrink_mib);
    if (result == PARTITION_MANAGER_OK) {
        if (ui_refresh_volume() == 0) {
            ui_status(state, "Partition shrunk. The freed space is ready for a new partition.", COLOR_LIGHT_GREEN);
        } else {
            ui_status(state, "Partition metadata changed, but the FAT32 volume could not be remounted.", COLOR_LIGHT_RED);
        }
    } else if (result == PARTITION_MANAGER_ERROR_DATA_PRESENT) {
        ui_status(state, "Allocated data reaches the partition tail. Move or remove it before shrinking.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_ARGUMENT) {
        ui_status(state, "That amount is too large or would leave an invalid FAT32 volume.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_PROTECTED) {
        ui_status(state, "Only the mounted FAT32 primary partition can be shrunk.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
        ui_status(state, "A whole-disk FAT32 volume cannot be shrunk as an MBR partition.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_TABLE) {
        ui_status(state, "Cannot shrink an invalid or unsupported MBR layout.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_PARTIAL) {
        ui_status(state, "FAT32 is safely smaller, but MBR update failed. Retry the same shrink amount.", COLOR_LIGHT_RED);
    } else {
        ui_status(state, "Disk read/write failed while shrinking the partition.", COLOR_LIGHT_RED);
    }
}

static void ui_rename_volume(PartitionUiState* state) {
    uint32_t selected_index;
    if (partition_manager_get_selected_index(&selected_index) != 0
        || selected_index != state->selected_slot + 1U) {
        ui_status(state, "Select the mounted FAT32 partition to change its label.", COLOR_LIGHT_RED);
        return;
    }
    if (vfs_set_volume_label(state->input) != 0) {
        ui_status(state, "Invalid label or write failed. Use 1-11 letters, digits, '_' or '-'.", COLOR_LIGHT_RED);
        return;
    }
    ui_status(state, "Volume label updated successfully.", COLOR_LIGHT_GREEN);
}

static void ui_format_partition(PartitionUiState* state) {
    partition_info_t partition;
    int result;

    if (partition_manager_is_superfloppy()
        || partition_manager_get(state->selected_slot, &partition) != 0
        || !partition.valid || (partition.type != 0x0BU && partition.type != 0x0CU)) {
        ui_status(state, "Select a valid FAT32 primary partition to format.", COLOR_LIGHT_RED);
        return;
    }

    vfs_unmount();
    if (blockdev_set_partition(partition.start_sector, partition.sector_count) != 0) {
        (void)ui_refresh_volume();
        ui_status(state, "Could not select the target partition; disk was not formatted.", COLOR_LIGHT_RED);
        return;
    }

    result = fat32_format_volume("PRISMOS", partition.start_sector);
    if (ui_refresh_volume() != 0) {
        ui_status(state, result == FAT32_RESIZE_OK
            ? "Formatted, but no FAT32 volume could be mounted afterward."
            : "Format failed and the mounted volume could not be restored.", COLOR_LIGHT_RED);
        return;
    }
    if (result == FAT32_RESIZE_OK) {
        ui_status(state, "Partition formatted as FAT32 with label PRISMOS.", COLOR_LIGHT_GREEN);
    } else if (result == FAT32_RESIZE_ERROR_INVALID) {
        ui_status(state, "Cannot format: partition geometry cannot be represented as FAT32.", COLOR_LIGHT_RED);
    } else {
        ui_status(state, "Disk I/O failed during format; check the volume before use.", COLOR_LIGHT_RED);
    }
}

static void ui_mount_partition(PartitionUiState* state) {
    uint32_t previous_index = 0U;
    int had_previous = partition_manager_get_selected_index(&previous_index) == 0;
    int result;

    vfs_unmount();
    result = partition_manager_select(state->selected_slot + 1U);
    if (result == PARTITION_MANAGER_OK && vfs_init() == 0) {
        ui_status(state, "Selected partition mounted as the active FAT32 volume.", COLOR_LIGHT_GREEN);
        return;
    }

    vfs_unmount();
    if (had_previous) {
        (void)partition_manager_select(previous_index);
        (void)vfs_init();
    } else {
        (void)ui_refresh_volume();
    }
    if (result == PARTITION_MANAGER_ERROR_UNSUPPORTED) {
        ui_status(state, "This partition is not formatted with a supported FAT32 volume.", COLOR_LIGHT_RED);
    } else if (result == PARTITION_MANAGER_ERROR_SUPERFLOPPY) {
        ui_status(state, "A whole-disk FAT32 volume cannot be selected as an MBR partition.", COLOR_LIGHT_RED);
    } else {
        ui_status(state, "Could not mount the selected partition; previous volume restored.", COLOR_LIGHT_RED);
    }
}

static void ui_handle_modal_event(PartitionUiState* state, KeyEvent event) {
    if (event.type == KEY_EVENT_CHARACTER && event.character == 27) {
        state->modal = UI_MODAL_NONE;
        return;
    }

    if (state->modal == UI_MODAL_DELETE) {
        if (event.type == KEY_EVENT_CHARACTER
            && (event.character == 'y' || event.character == 'Y')) {
            ui_delete_partition(state);
        } else {
            ui_status(state, "Delete cancelled.", COLOR_LIGHT_CYAN);
        }
        state->modal = UI_MODAL_NONE;
        return;
    }
    if (state->modal == UI_MODAL_FORMAT) {
        if (event.type == KEY_EVENT_CHARACTER
            && (event.character == 'y' || event.character == 'Y')) {
            ui_format_partition(state);
        } else {
            ui_status(state, "Format cancelled; partition data was not changed.", COLOR_LIGHT_CYAN);
        }
        state->modal = UI_MODAL_NONE;
        return;
    }

    if (event.type == KEY_EVENT_BACKSPACE) {
        if (state->input_length > 0U) {
            state->input[--state->input_length] = '\0';
        }
        return;
    }
    if (event.type == KEY_EVENT_ENTER) {
        if (state->modal == UI_MODAL_CREATE) {
            uint32_t ignored_size;
            if (ui_parse_size(state->input, &ignored_size) != 0) {
                ui_status(state, "Enter a positive whole-number size in MiB.", COLOR_LIGHT_RED);
                return;
            }
            ui_create_partition(state);
        } else if (state->modal == UI_MODAL_SHRINK) {
            uint32_t ignored_size;
            if (ui_parse_size(state->input, &ignored_size) != 0) {
                ui_status(state, "Enter a positive whole-number shrink amount in MiB.", COLOR_LIGHT_RED);
                return;
            }
            ui_shrink_partition(state);
        } else if (state->modal == UI_MODAL_RENAME) {
            if (state->input_length == 0U) {
                ui_status(state, "Enter a volume label before accepting.", COLOR_LIGHT_RED);
                return;
            }
            ui_rename_volume(state);
        }
        state->modal = UI_MODAL_NONE;
        return;
    }
    if (event.type == KEY_EVENT_CHARACTER && state->input_length + 1U < sizeof(state->input)) {
        char character = event.character;
        int accepted = 0;
        if (state->modal == UI_MODAL_CREATE || state->modal == UI_MODAL_SHRINK) {
            accepted = character >= '0' && character <= '9';
        } else if (state->modal == UI_MODAL_RENAME) {
            accepted = (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-';
        }
        if (accepted) {
            state->input[state->input_length++] = character;
        }
        state->input[state->input_length] = '\0';
    }
}

static int ui_has_mounted_partition(void) {
    uint32_t mounted_index;
    return partition_manager_get_selected_index(&mounted_index) == 0;
}

int partition_manager_app_run(void) {
    PartitionUiState state = {0};
    uint32_t width = console_get_framebuffer_width();
    uint32_t height = console_get_framebuffer_height();

    state.columns = width / 8U;
    state.rows = height / 16U;
    state.status_color = COLOR_LIGHT_CYAN;
    state.modal = UI_MODAL_NONE;
    state.running = 1;
    ui_status(&state, "Ready. Select a slot, then choose an action.", COLOR_LIGHT_CYAN);

    if (blockdev_drive_count() == 0U || state.columns < UI_MIN_COLUMNS || state.rows < UI_MIN_ROWS) {
        console_writeln("Partition manager needs an ATA drive and a larger display.");
        return -1;
    }
    {
        uint32_t mounted_index;
        if (partition_manager_get_selected_index(&mounted_index) == 0 && mounted_index > 0U) {
            state.selected_slot = mounted_index - 1U;
        }
    }

        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        console_clear();
    while (state.running) {
        KeyEvent event;
        ui_draw(&state);
        event = keyboard_read_event();

        if (state.modal != UI_MODAL_NONE) {
            ui_handle_modal_event(&state, event);
            continue;
        }

        if (event.type == KEY_EVENT_UP) {
            state.selected_slot = state.selected_slot == 0U
                ? PARTITION_MANAGER_MAX_PARTITIONS - 1U : state.selected_slot - 1U;
        } else if (event.type == KEY_EVENT_DOWN) {
            state.selected_slot = (state.selected_slot + 1U) % PARTITION_MANAGER_MAX_PARTITIONS;
        } else if (event.type == KEY_EVENT_CHARACTER) {
            char character = event.character;
            if (character >= 'A' && character <= 'Z') {
                character = (char)(character - 'A' + 'a');
            }
            if (character == 27 || character == 'q') {
                state.running = 0;
            } else if (character == 'c') {
                if (partition_manager_is_superfloppy()) {
                    ui_status(&state, "Partition editing is disabled for a superfloppy disk.", COLOR_LIGHT_RED);
                } else if (!ui_has_mounted_partition()) {
                    ui_status(&state, "Mount a supported FAT32 volume before editing this disk.", COLOR_LIGHT_RED);
                } else {
                    ui_open_modal(&state, UI_MODAL_CREATE);
                }
            } else if (character == 'd') {
                if (partition_manager_is_superfloppy()) {
                    ui_status(&state, "Partition editing is disabled for a superfloppy disk.", COLOR_LIGHT_RED);
                } else if (!ui_has_mounted_partition()) {
                    ui_status(&state, "Mount a supported FAT32 volume before editing this disk.", COLOR_LIGHT_RED);
                } else {
                    ui_open_modal(&state, UI_MODAL_DELETE);
                }
            } else if (character == 's') {
                uint32_t mounted_index;
                if (partition_manager_is_superfloppy()) {
                    ui_status(&state, "Partition editing is disabled for a superfloppy disk.", COLOR_LIGHT_RED);
                } else if (partition_manager_get_selected_index(&mounted_index) != 0
                    || mounted_index != state.selected_slot + 1U) {
                    ui_status(&state, "Select the mounted FAT32 partition to shrink it.", COLOR_LIGHT_RED);
                } else {
                    ui_open_modal(&state, UI_MODAL_SHRINK);
                }
            } else if (character == 'r') {
                uint32_t mounted_index;
                if (partition_manager_get_selected_index(&mounted_index) == 0
                    && mounted_index == state.selected_slot + 1U) {
                    ui_open_modal(&state, UI_MODAL_RENAME);
                } else {
                    ui_status(&state, "Select the mounted FAT32 partition to rename its volume label.", COLOR_LIGHT_RED);
                }
            } else if (character == 'f') {
                partition_info_t partition;
                if (partition_manager_is_superfloppy()) {
                    ui_status(&state, "Formatting a whole-disk FAT32 volume is not supported here.", COLOR_LIGHT_RED);
                } else if (partition_manager_get(state.selected_slot, &partition) != 0
                    || !partition.valid || (partition.type != 0x0BU && partition.type != 0x0CU)) {
                    ui_status(&state, "Select a valid FAT32 primary partition to format.", COLOR_LIGHT_RED);
                } else {
                    ui_open_modal(&state, UI_MODAL_FORMAT);
                }
            } else if (character == 'm') {
                ui_mount_partition(&state);
            }
        }
    }

    console_set_cursor_visible(1);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    console_set_cursor(0, 0);
    return 0;
}