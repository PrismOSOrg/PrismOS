#include "apps/help_app.h"

#include <stdint.h>

#include "commands/command.h"
#include "debug/log.h"
#include "display/console.h"
#include "input/keyboard.h"

#define HELP_MIN_COLUMNS 48U
#define HELP_MIN_ROWS 18U
#define HELP_LIST_WIDTH 26U

typedef struct {
    uint32_t selected;
    uint32_t first_visible;
    uint32_t columns;
    uint32_t rows;
    uint32_t list_width;
    uint32_t content_rows;
    int detail_open;
    int running;
} HelpState;

static void write_clipped(const char* text, uint32_t width) {
    uint32_t count = 0;
    while (text[count] != '\0' && count < width) {
        console_write_char(text[count]);
        count++;
    }
}

static void fill_row(uint32_t row, VGA_Color fg, VGA_Color bg) {
    console_set_color(fg, bg);
    console_clear_row((int)row);
}

static void fill_area(uint32_t x, uint32_t row, uint32_t width, uint32_t height, VGA_Color fg, VGA_Color bg) {
    console_set_color(fg, bg);
    for (uint32_t line = 0; line < height; line++) {
        console_set_cursor((int)x, (int)(row + line));
        for (uint32_t column = 0; column < width; column++) {
            console_write_char(' ');
        }
    }
}

static void draw_bar(uint32_t row, const char* text, VGA_Color fg, VGA_Color bg) {
    fill_row(row, fg, bg);
    console_set_cursor(0, (int)row);
    write_clipped(text, console_get_framebuffer_width() / 8U);
}

static uint32_t draw_wrapped(const char* text,
    uint32_t x,
    uint32_t row,
    uint32_t width,
    uint32_t last_row) {
    uint32_t column = 0;

    while (*text != '\0' && row < last_row) {
        uint32_t word_length = 0;

        if (*text == '\n') {
            row++;
            column = 0;
            text++;
            continue;
        }

        if (*text == ' ') {
            if (column > 0U && column < width) {
                console_set_cursor((int)(x + column), (int)row);
                console_write_char(' ');
                column++;
            }
            text++;
            continue;
        }

        while (text[word_length] != '\0' && text[word_length] != ' ' && text[word_length] != '\n') {
            word_length++;
        }

        if (column > 0U && column + word_length > width) {
            row++;
            column = 0;
        }

        while (word_length > 0U && row < last_row) {
            if (column >= width) {
                row++;
                column = 0;
                if (row >= last_row) {
                    break;
                }
            }

            console_set_cursor((int)(x + column), (int)row);
            console_write_char(*text++);
            column++;
            word_length--;
        }
    }

    return row;
}

static void draw_list_row(const HelpState* state, uint32_t offset) {
    uint32_t count = command_get_help_count();
    uint32_t command_index = state->first_visible + offset;
    uint32_t row = offset + 2U;

    if (row >= state->rows - 1U) {
        return;
    }

    if (command_index < count) {
        const command_help_entry_t* entry = command_get_help_entry(command_index);
        VGA_Color fg = command_index == state->selected ? COLOR_WHITE : COLOR_LIGHT_GRAY;
        VGA_Color bg = command_index == state->selected ? COLOR_BLUE : COLOR_BLACK;

        fill_area(0U, row, state->list_width, 1U, fg, bg);
        console_set_color(fg, bg);
        console_set_cursor(0, (int)row);
        console_write(command_index == state->selected ? " > " : "   ");
        write_clipped(entry->name, state->list_width - 3U);
    } else {
        fill_area(0U, row, state->list_width, 1U, COLOR_LIGHT_GRAY, COLOR_BLACK);
    }

    console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    console_set_cursor((int)state->list_width, (int)row);
    console_write_char('|');
}

static void draw_index_preview(const HelpState* state) {
    uint32_t detail_x = state->list_width + 2U;
    uint32_t detail_width = state->columns > detail_x ? state->columns - detail_x : 0U;
    uint32_t panel_height = state->rows > 4U ? state->rows - 4U : 0U;
    const command_help_entry_t* selected = command_get_help_entry(state->selected);

    if (detail_width == 0U || panel_height == 0U) {
        return;
    }

    fill_area(detail_x, 2U, detail_width, panel_height, COLOR_LIGHT_GRAY, COLOR_BLACK);
    if (selected != 0) {
        uint32_t row = 3U;
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_set_cursor((int)detail_x, (int)row++);
        write_clipped(selected->name, detail_width);

        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_set_cursor((int)detail_x, (int)row++);
        console_write("Description:");
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        row = draw_wrapped(selected->description, detail_x, row, detail_width, state->rows - 2U) + 2U;

        if (row < state->rows - 2U) {
            console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            console_set_cursor((int)detail_x, (int)row++);
            console_write("Syntax:");
            console_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            draw_wrapped(selected->usage, detail_x, row, detail_width, state->rows - 2U);
        }

        if (row + 1U < state->rows - 2U) {
            console_set_color(COLOR_DARK_GRAY, COLOR_BLACK);
            console_set_cursor((int)detail_x, (int)(state->rows - 3U));
            console_write("Enter opens full help; type a letter to jump.");
        }
    }
}

static void draw_index(const HelpState* state) {
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    console_set_cursor_visible(0);
    draw_bar(0U, " PrismOS Help  |  Command Index", COLOR_WHITE, COLOR_BLUE);

    fill_row(1U, COLOR_BLACK, COLOR_LIGHT_GRAY);
    console_set_cursor(0, 1);
    console_write(" COMMANDS");
    console_set_cursor((int)(state->list_width + 1U), 1);
    console_write(" DETAILS");

    for (uint32_t offset = 0; offset < state->content_rows; offset++) {
        draw_list_row(state, offset);
    }
    draw_index_preview(state);

    draw_bar(state->rows - 1U,
        " Up/Down select  Enter open  Type a letter to jump  Esc/Q close",
        COLOR_BLACK,
        COLOR_LIGHT_GRAY);
}

static void draw_detail_content(const HelpState* state) {
    const command_help_entry_t* entry = command_get_help_entry(state->selected);
    uint32_t width = state->columns > 4U ? state->columns - 4U : 1U;
    uint32_t row = 2U;

    fill_area(0U, 1U, state->columns, state->rows - 2U, COLOR_LIGHT_GRAY, COLOR_BLACK);

    if (entry != 0) {
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_set_cursor(2, (int)row++);
        console_write(entry->name);

        row++;
        console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        console_set_cursor(2, (int)row++);
        console_write("Description");
        console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        row = draw_wrapped(entry->description, 2U, row, width, state->rows - 2U) + 2U;

        if (row < state->rows - 2U) {
            console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            console_set_cursor(2, (int)row++);
            console_write("Syntax");
            console_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            row = draw_wrapped(entry->usage, 2U, row, width, state->rows - 2U) + 2U;
        }

        if (row < state->rows - 2U) {
            console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            console_set_cursor(2, (int)row++);
            console_write("Parameters");
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            row = draw_wrapped(entry->parameters, 2U, row, width, state->rows - 2U) + 2U;
        }

        if (row < state->rows - 2U) {
            console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
            console_set_cursor(2, (int)row++);
            console_write("Example");
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            draw_wrapped(entry->example, 2U, row, width, state->rows - 2U);
        }
    }

}

static void draw_detail(const HelpState* state) {
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    console_clear();
    console_set_cursor_visible(0);
    draw_bar(0U, " PrismOS Help  |  Command Details", COLOR_WHITE, COLOR_BLUE);
    draw_detail_content(state);
    draw_bar(state->rows - 1U,
        " Esc/Enter return to index  Up/Down browse  Q close help",
        COLOR_BLACK,
        COLOR_LIGHT_GRAY);
}

static void help_move_selection(HelpState* state, int direction) {
    uint32_t count = command_get_help_count();

    if (count == 0U) {
        state->selected = 0U;
        state->first_visible = 0U;
        return;
    }

    if (direction < 0) {
        state->selected = state->selected == 0U ? count - 1U : state->selected - 1U;
    } else {
        state->selected = state->selected + 1U >= count ? 0U : state->selected + 1U;
    }

    if (state->selected < state->first_visible) {
        state->first_visible = state->selected;
    } else if (state->selected >= state->first_visible + state->content_rows) {
        state->first_visible = state->selected - state->content_rows + 1U;
    }
}

static void help_jump_to_letter(HelpState* state, char character) {
    uint32_t count = command_get_help_count();
    uint32_t start = state->selected + 1U;

    if (character >= 'A' && character <= 'Z') {
        character = (char)(character - 'A' + 'a');
    }

    for (uint32_t offset = 0; offset < count; offset++) {
        uint32_t index = (start + offset) % count;
        const command_help_entry_t* entry = command_get_help_entry(index);
        char first = entry->name[0];

        if (first >= 'A' && first <= 'Z') {
            first = (char)(first - 'A' + 'a');
        }
        if (first == character) {
            state->selected = index;
            if (state->selected < state->first_visible) {
                state->first_visible = state->selected;
            } else if (state->selected >= state->first_visible + state->content_rows) {
                state->first_visible = state->selected - state->content_rows + 1U;
            }
            return;
        }
    }
}

int help_app_run(void) {
    HelpState state = {0};
    uint32_t width = console_get_framebuffer_width();
    uint32_t height = console_get_framebuffer_height();
    uint32_t count = command_get_help_count();

    state.columns = width / 8U;
    state.rows = height / 16U;
    state.list_width = HELP_LIST_WIDTH;
    state.content_rows = state.rows > 3U ? state.rows - 3U : 1U;
    state.detail_open = 0;
    state.running = 1;

    if (count == 0U) {
        console_writeln("No commands are registered.");
        return -1;
    }
    if (state.columns < HELP_MIN_COLUMNS || state.rows < HELP_MIN_ROWS) {
        console_writeln("Help browser needs a larger display.");
        command_print_help();
        return -1;
    }
    if (state.list_width >= state.columns / 2U) {
        state.list_width = state.columns / 3U;
    }

    DEBUG_LOG("help browser opened");
    draw_index(&state);
    while (state.running) {
        KeyEvent event;
        uint32_t previous_selected = state.selected;
        uint32_t previous_first_visible = state.first_visible;
        int previous_detail_open = state.detail_open;

        event = keyboard_read_event();
        if (event.type == KEY_EVENT_CHARACTER) {
            char character = event.character;
            if (character == 27 || character == 'q' || character == 'Q') {
                if (state.detail_open && (character == 27)) {
                    state.detail_open = 0;
                } else {
                    state.running = 0;
                }
            } else if (!state.detail_open && character >= 32 && character <= 126) {
                help_jump_to_letter(&state, character);
            }
        } else {
            switch (event.type) {
                case KEY_EVENT_UP:
                    help_move_selection(&state, -1);
                    break;
                case KEY_EVENT_DOWN:
                    help_move_selection(&state, 1);
                    break;
                case KEY_EVENT_HOME:
                    state.selected = 0U;
                    state.first_visible = 0U;
                    break;
                case KEY_EVENT_END:
                    state.selected = count - 1U;
                    state.first_visible = count > state.content_rows ? count - state.content_rows : 0U;
                    break;
                case KEY_EVENT_ENTER:
                    state.detail_open = !state.detail_open;
                    break;
                case KEY_EVENT_NONE:
                case KEY_EVENT_BACKSPACE:
                case KEY_EVENT_DELETE:
                case KEY_EVENT_LEFT:
                case KEY_EVENT_RIGHT:
                case KEY_EVENT_COPY:
                case KEY_EVENT_PASTE:
                default:
                    break;
            }
        }

        if (!state.running) {
            break;
        }

        if (previous_detail_open != state.detail_open) {
            if (state.detail_open) {
                draw_detail(&state);
            } else {
                draw_index(&state);
            }
        } else if (previous_selected != state.selected || previous_first_visible != state.first_visible) {
            if (state.detail_open) {
                draw_detail_content(&state);
            } else {
                if (previous_first_visible != state.first_visible) {
                    for (uint32_t offset = 0; offset < state.content_rows; offset++) {
                        draw_list_row(&state, offset);
                    }
                } else {
                    if (previous_selected >= state.first_visible
                        && previous_selected < state.first_visible + state.content_rows) {
                        draw_list_row(&state, previous_selected - state.first_visible);
                    }
                    if (state.selected >= state.first_visible
                        && state.selected < state.first_visible + state.content_rows) {
                        draw_list_row(&state, state.selected - state.first_visible);
                    }
                }
                draw_index_preview(&state);
            }
        }
    }

    console_set_color(COLOR_WHITE, COLOR_BLACK);
    console_clear();
    DEBUG_LOG("help browser closed");
    return 0;
}