#include "apps/package_manager.h"

#include "display/console.h"
#include "input/keyboard.h"

typedef struct {
    uint32_t columns;
    uint32_t rows;
    uint32_t selected;
    uint32_t first_visible;
    int details_open;
    int confirm_remove;
    char status[160];
    char description[121];
    VGA_Color status_color;
} PackageUiState;

static void ui_copy(char* destination, uint32_t capacity, const char* source) {
    uint32_t index = 0U;
    if (capacity == 0U) return;
    while (source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static char ui_lower(char character) {
    return character >= 'A' && character <= 'Z'
        ? (char)(character - 'A' + 'a') : character;
}

static void ui_write_clipped(const char* text, uint32_t width) {
    for (uint32_t i = 0U; text[i] != '\0' && i < width; i++) console_write_char(text[i]);
}

static void ui_row(const PackageUiState* state, uint32_t row, VGA_Color foreground,
    VGA_Color background, const char* text) {
    console_set_color(foreground, background);
    console_clear_row((int)row);
    console_set_cursor(1, (int)row);
    ui_write_clipped(text, state->columns - 2U);
}

static void ui_set_status(PackageUiState* state, const char* text, VGA_Color color) {
    ui_copy(state->status, sizeof(state->status), text);
    state->status_color = color;
}

static void ui_draw_entry(PackageUiState* state, uint32_t index, uint32_t row) {
    package_manager_entry_t entry;
    char line[160];
    uint32_t length = 0U;
    VGA_Color foreground = index == state->selected ? COLOR_WHITE : COLOR_LIGHT_GRAY;
    VGA_Color background = index == state->selected ? COLOR_BLUE : COLOR_BLACK;
    if (package_manager_get_entry(index, &entry) != 0) return;
    line[0] = '\0';
#define UI_APPEND(text) do { \
    const char* part = (text); \
    while (*part != '\0' && length + 1U < sizeof(line)) line[length++] = *part++; \
    line[length] = '\0'; \
} while (0)
    UI_APPEND(index == state->selected ? "> " : "  ");
    UI_APPEND(entry.favorite ? "* " : "  ");
    UI_APPEND(entry.id); UI_APPEND("  "); UI_APPEND(entry.version); UI_APPEND("  ");
    console_set_color(foreground, background);
    console_clear_row((int)row);
    console_set_cursor(1, (int)row);
    ui_write_clipped(line, state->columns - 2U);
    console_set_color(COLOR_DARK_GRAY, background);
    console_set_cursor((int)(state->columns > 22U ? state->columns - 21U : 1U), (int)row);
    console_write_uint(entry.size);
    console_write(" B  ");
    console_set_color(entry.installed ? COLOR_LIGHT_GREEN : COLOR_YELLOW, background);
    ui_write_clipped(entry.installed ? "INSTALLED" : "AVAILABLE", 9U);
#undef UI_APPEND
}

static void ui_draw_list(PackageUiState* state) {
    uint32_t count = package_manager_count();
    uint32_t list_capacity = state->rows > 8U ? state->rows - 8U : 1U;
    uint32_t end = state->first_visible + list_capacity;
    console_set_cursor_visible(0);
    ui_row(state, 0U, COLOR_WHITE, COLOR_BLUE, " PrismOS   PACKAGE MANAGER");
    ui_row(state, 1U, COLOR_LIGHT_CYAN, COLOR_BLACK,
        " Browse, inspect, install and remove PrismOS application packages");
    ui_row(state, 2U, COLOR_DARK_GRAY, COLOR_BLACK,
        " Repository: 10.0.2.2:8080  |  HTTP only; SHA-256 checks integrity, not publisher identity");
    ui_row(state, 3U, COLOR_LIGHT_CYAN, COLOR_BLACK,
        " SELECT  * favorite   ID / VERSION                                  SIZE       STATUS");
    for (uint32_t line = 0U; line < list_capacity; line++) {
        uint32_t row = 4U + line;
        uint32_t index = state->first_visible + line;
        if (index < count && index < end) ui_draw_entry(state, index, row);
        else { console_clear_row((int)row); }
    }
    ui_row(state, state->rows - 3U, state->status_color, COLOR_BLACK, state->status);
    ui_row(state, state->rows - 2U, COLOR_LIGHT_GRAY, COLOR_BLACK,
        " Up/Down select  Enter details  I install  U remove  F favorite  R refresh");
    ui_row(state, state->rows - 1U, COLOR_BLACK, COLOR_LIGHT_GRAY,
        " Esc / Q exit                                           PrismOS Package Manager ");
}

static void ui_draw_details(PackageUiState* state) {
    package_manager_entry_t entry;
    if (package_manager_get_entry(state->selected, &entry) != 0) return;
    console_set_cursor_visible(0);
    ui_row(state, 0U, COLOR_WHITE, COLOR_BLUE, " PrismOS   PACKAGE DETAILS");
    ui_row(state, 1U, COLOR_LIGHT_CYAN, COLOR_BLACK, " Metadata is retrieved from the repository manifest");
    ui_row(state, 3U, COLOR_YELLOW, COLOR_BLACK, " PACKAGE");
    console_set_color(COLOR_WHITE, COLOR_BLACK);
    console_set_cursor(2, 4); console_write("ID          "); console_writeln(entry.id);
    console_set_cursor(2, 5); console_write("Version     "); console_writeln(entry.version);
    console_set_cursor(2, 6); console_write("Size        "); console_write_uint(entry.size); console_writeln(" bytes");
    console_set_cursor(2, 7); console_write("State       "); console_writeln(entry.installed ? "Installed" : "Not installed");
    console_set_cursor(2, 8); console_write("Favorite    "); console_writeln(entry.favorite ? "Yes" : "No");
    console_set_cursor(2, 10); console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_writeln("Description");
    console_set_cursor(2, 11); console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    ui_write_clipped(state->description[0] != '\0' ? state->description : "(no description)", state->columns - 4U);
    console_set_cursor(2, 13); console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    console_write("SHA-256: "); ui_write_clipped(entry.sha256, state->columns - 12U);
    ui_row(state, state->rows - 3U, state->status_color, COLOR_BLACK, state->status);
    ui_row(state, state->rows - 2U, COLOR_LIGHT_GRAY, COLOR_BLACK,
        " I install  U remove  F favorite");
    ui_row(state, state->rows - 1U, COLOR_BLACK, COLOR_LIGHT_GRAY, " Esc / Enter back");
}

static void ui_refresh(PackageUiState* state) {
    ui_set_status(state, "Contacting the repository ...", COLOR_YELLOW);
    ui_draw_list(state);
    int result = package_manager_refresh();
    if (result == PACKAGE_MANAGER_OK) {
        if (state->selected >= package_manager_count()) state->selected = 0U;
        state->first_visible = 0U;
        ui_set_status(state, "Catalog refreshed.", COLOR_LIGHT_GREEN);
    } else {
        ui_set_status(state, package_manager_error_text(result), COLOR_LIGHT_RED);
    }
}

static void ui_show_details(PackageUiState* state) {
    ui_set_status(state, "Loading package manifest ...", COLOR_YELLOW);
    ui_draw_list(state);
    int result = package_manager_get_details(state->selected, state->description,
        sizeof(state->description));
    if (result != PACKAGE_MANAGER_OK) {
        ui_set_status(state, package_manager_error_text(result), COLOR_LIGHT_RED);
        return;
    }
    ui_set_status(state, "Manifest verified.", COLOR_LIGHT_GREEN);
    state->details_open = 1;
}

static void ui_install(PackageUiState* state) {
    ui_set_status(state, "Downloading chunks, checking SHA-256 and app format ...", COLOR_YELLOW);
    ui_draw_list(state);
    int result = package_manager_install(state->selected);
    ui_set_status(state, package_manager_error_text(result),
        result == PACKAGE_MANAGER_OK ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED);
}

static void ui_remove(PackageUiState* state) {
    package_manager_entry_t entry;
    if (package_manager_get_entry(state->selected, &entry) != 0 || !entry.installed) {
        ui_set_status(state, "Selected package version is not installed.", COLOR_YELLOW);
        state->confirm_remove = 0;
        return;
    }
    if (state->confirm_remove == 0) {
        state->confirm_remove = 1;
        ui_set_status(state, "Press U again to uninstall the selected package.", COLOR_YELLOW);
        return;
    }
    int result = package_manager_remove(state->selected);
    state->confirm_remove = 0;
    ui_set_status(state, package_manager_error_text(result),
        result == PACKAGE_MANAGER_OK ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED);
}

static void ui_favorite(PackageUiState* state) {
    int result = package_manager_toggle_favorite(state->selected);
    ui_set_status(state, result == PACKAGE_MANAGER_OK ? "Favorite updated on this drive."
        : package_manager_error_text(result), result == PACKAGE_MANAGER_OK
        ? COLOR_LIGHT_GREEN : COLOR_LIGHT_RED);
}

void package_manager_run(void) {
    PackageUiState state = {0};
    state.columns = console_get_framebuffer_width() / 8U;
    state.rows = console_get_framebuffer_height() / 16U;
    state.status_color = COLOR_LIGHT_CYAN;
    ui_copy(state.status, sizeof(state.status), "Loading package catalog ...");
    if (state.columns < 64U || state.rows < 20U) {
        console_writeln("Package Manager needs a display of at least 64x20 text cells.");
        return;
    }
    ui_draw_list(&state);
    int refresh_result = package_manager_refresh();
    if (refresh_result != PACKAGE_MANAGER_OK)
        ui_set_status(&state, package_manager_error_text(refresh_result), COLOR_LIGHT_RED);
    else if (package_manager_count() == 0U)
        ui_set_status(&state, "No packages are available. Press R to refresh.", COLOR_YELLOW);
    else ui_set_status(&state, "Catalog loaded. Select a package with Up/Down.", COLOR_LIGHT_GREEN);

    for (;;) {
        if (state.details_open) ui_draw_details(&state);
        else ui_draw_list(&state);
        KeyEvent event = keyboard_read_event();
        if (state.details_open) {
            if (event.type == KEY_EVENT_ENTER || (event.type == KEY_EVENT_CHARACTER
                    && event.character == 27)) state.details_open = 0;
            else if (event.type == KEY_EVENT_CHARACTER) {
                char key = ui_lower(event.character);
                if (key == 'i') ui_install(&state);
                else if (key == 'u') ui_remove(&state);
                else if (key == 'f') ui_favorite(&state);
            }
            continue;
        }
        if (event.type == KEY_EVENT_CHARACTER && (event.character == 27
                || ui_lower(event.character) == 'q')) break;
        if (event.type == KEY_EVENT_UP && state.selected > 0U) { state.selected--; state.confirm_remove = 0; }
        else if (event.type == KEY_EVENT_DOWN && state.selected + 1U < package_manager_count()) { state.selected++; state.confirm_remove = 0; }
        else if (event.type == KEY_EVENT_HOME) state.selected = 0U;
        else if (event.type == KEY_EVENT_END && package_manager_count() != 0U)
            state.selected = package_manager_count() - 1U;
        else if (event.type == KEY_EVENT_ENTER && package_manager_count() != 0U) ui_show_details(&state);
        else if (event.type == KEY_EVENT_CHARACTER) {
            char key = ui_lower(event.character);
            if (key == 'r') { state.confirm_remove = 0; ui_refresh(&state); }
            else if (key == 'i' && package_manager_count() != 0U) { state.confirm_remove = 0; ui_install(&state); }
            else if (key == 'u' && package_manager_count() != 0U) ui_remove(&state);
            else if (key == 'f' && package_manager_count() != 0U) { state.confirm_remove = 0; ui_favorite(&state); }
        }
        uint32_t capacity = state.rows - 8U;
        if (state.selected < state.first_visible) state.first_visible = state.selected;
        if (state.selected >= state.first_visible + capacity)
            state.first_visible = state.selected - capacity + 1U;
        if (!state.confirm_remove && state.status_color == COLOR_YELLOW
            && event.type == KEY_EVENT_UP) ui_set_status(&state, "", COLOR_LIGHT_CYAN);
    }
    console_set_cursor_visible(1);
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
}