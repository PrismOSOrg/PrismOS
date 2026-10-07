#include "apps/editor_app.h"

#include <stdint.h>

#include "debug/log.h"
#include "display/console.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"
#include "util/clipboard.h"

#define EDITOR_MAX_TEXT (16U * 1024U)
#define EDITOR_MAX_PATH 128U
#define EDITOR_MAX_VISIBLE_ROWS 128U

typedef struct {
    char path[EDITOR_MAX_PATH];
    char text[EDITOR_MAX_TEXT + 1U];
    uint32_t length;
    uint32_t cursor;
    uint32_t selection_anchor;
    uint32_t preferred_col;
    uint32_t scroll_row;
    int modified;
    int running;
    uint32_t prev_cursor_row;
    uint32_t prev_cursor_col;
    uint32_t prev_scroll_row;
    uint32_t prev_length;
    uint32_t prev_columns;
    uint32_t prev_rows;
    int prev_modified;
    int full_redraw;
    uint32_t prev_visible_rows;
    uint32_t prev_row_signature[EDITOR_MAX_VISIBLE_ROWS];
    uint8_t prev_row_valid[EDITOR_MAX_VISIBLE_ROWS];
} EditorState;

static void copy_limited(char* dst, uint32_t capacity, const char* src) {
    uint32_t i = 0;
    if (capacity == 0U) {
        return;
    }

    while (src[i] != '\0' && i < (capacity - 1U)) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static uint32_t editor_columns(void) {
    uint32_t width = console_get_framebuffer_width();
    if (width < 8U) {
        return 0;
    }

    return width / 8U;
}

static uint32_t editor_rows(void) {
    uint32_t height = console_get_framebuffer_height();
    if (height < 16U) {
        return 0;
    }

    return height / 16U;
}

static int editor_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

static uint32_t line_start_for_index(const EditorState* state, uint32_t index) {
    uint32_t pos = index;
    if (pos > state->length) {
        pos = state->length;
    }

    while (pos > 0U && state->text[pos - 1U] != '\n') {
        pos--;
    }

    return pos;
}

static uint32_t line_end_for_start(const EditorState* state, uint32_t start) {
    uint32_t pos = start;
    while (pos < state->length && state->text[pos] != '\n') {
        pos++;
    }

    return pos;
}

static uint32_t line_start_for_row(const EditorState* state, uint32_t row) {
    uint32_t current_row = 0;
    uint32_t pos = 0;

    while (pos < state->length && current_row < row) {
        if (state->text[pos] == '\n') {
            current_row++;
        }
        pos++;
    }

    return pos;
}

static uint32_t column_for_index(const EditorState* state, uint32_t index) {
    uint32_t start = line_start_for_index(state, index);
    return index - start;
}

static void cursor_metrics_for_index(const EditorState* state, uint32_t index, uint32_t* out_row, uint32_t* out_col) {
    uint32_t row = 0;
    uint32_t col = 0;
    uint32_t capped = index;

    if (capped > state->length) {
        capped = state->length;
    }

    for (uint32_t i = 0; i < capped; i++) {
        if (state->text[i] == '\n') {
            row++;
            col = 0;
        } else {
            col++;
        }
    }

    *out_row = row;
    *out_col = col;
}

static int ensure_cursor_visible(EditorState* state, uint32_t row) {
    uint32_t visible_rows = editor_rows();
    uint32_t old_scroll = state->scroll_row;

    if (visible_rows < 3U) {
        state->scroll_row = 0;
        return state->scroll_row != old_scroll;
    }

    visible_rows -= 2U;

    if (row < state->scroll_row) {
        state->scroll_row = row;
    } else if (row >= state->scroll_row + visible_rows) {
        state->scroll_row = row - visible_rows + 1U;
    }

    return state->scroll_row != old_scroll;
}

static void editor_insert(EditorState* state, char c) {
    if (state->selection_anchor != 0xFFFFFFFFU) {
        uint32_t start = state->selection_anchor < state->cursor ? state->selection_anchor : state->cursor;
        uint32_t end = state->selection_anchor > state->cursor ? state->selection_anchor : state->cursor;
        for (uint32_t i = end; i <= state->length; i++) {
            state->text[start + i - end] = state->text[i];
        }
        state->length -= end - start;
        state->cursor = start;
        state->selection_anchor = 0xFFFFFFFFU;
    }

    if (state->length >= EDITOR_MAX_TEXT || state->cursor > state->length) {
        return;
    }

    for (uint32_t i = state->length; i > state->cursor; i--) {
        state->text[i] = state->text[i - 1U];
    }

    state->text[state->cursor] = c;
    state->length++;
    state->cursor++;
    state->text[state->length] = '\0';
    state->modified = 1;
}

static void editor_backspace(EditorState* state) {
    if (state->selection_anchor != 0xFFFFFFFFU && state->selection_anchor != state->cursor) {
        uint32_t start = state->selection_anchor < state->cursor ? state->selection_anchor : state->cursor;
        uint32_t end = state->selection_anchor > state->cursor ? state->selection_anchor : state->cursor;
        for (uint32_t i = end; i <= state->length; i++) {
            state->text[start + i - end] = state->text[i];
        }
        state->length -= end - start;
        state->cursor = start;
        state->selection_anchor = 0xFFFFFFFFU;
        state->modified = 1;
        return;
    }

    if (state->cursor == 0U || state->length == 0U) {
        return;
    }

    for (uint32_t i = state->cursor - 1U; i < state->length - 1U; i++) {
        state->text[i] = state->text[i + 1U];
    }

    state->length--;
    state->cursor--;
    state->text[state->length] = '\0';
    state->modified = 1;
}

static void editor_delete(EditorState* state) {
    if (state->selection_anchor != 0xFFFFFFFFU && state->selection_anchor != state->cursor) {
        editor_backspace(state);
        return;
    }

    if (state->cursor >= state->length || state->length == 0U) {
        return;
    }

    for (uint32_t i = state->cursor; i < state->length - 1U; i++) {
        state->text[i] = state->text[i + 1U];
    }

    state->length--;
    state->text[state->length] = '\0';
    state->modified = 1;
}

static void editor_move_vertical(EditorState* state, int direction) {
    uint32_t current_start = line_start_for_index(state, state->cursor);
    uint32_t current_col = state->cursor - current_start;
    uint32_t target_start;
    uint32_t target_end;
    uint32_t target_col;

    if (direction < 0) {
        if (current_start == 0U) {
            return;
        }

        target_start = line_start_for_index(state, current_start - 1U);
    } else {
        uint32_t end = line_end_for_start(state, current_start);
        if (end >= state->length) {
            return;
        }

        target_start = end + 1U;
    }

    if (state->preferred_col < current_col) {
        state->preferred_col = current_col;
    }

    target_end = line_end_for_start(state, target_start);
    target_col = state->preferred_col;
    if (target_col > (target_end - target_start)) {
        target_col = target_end - target_start;
    }

    state->cursor = target_start + target_col;
}

static void editor_move_left(EditorState* state, int word) {
    if (word) {
        while (state->cursor > 0U && editor_is_space(state->text[state->cursor - 1U])) {
            state->cursor--;
        }
        while (state->cursor > 0U && state->text[state->cursor - 1U] != ' ') {
            state->cursor--;
        }
    } else if (state->cursor > 0U) {
        state->cursor--;
    }
    state->preferred_col = column_for_index(state, state->cursor);
}

static void editor_move_right(EditorState* state, int word) {
    if (word) {
        while (state->cursor < state->length && editor_is_space(state->text[state->cursor])) {
            state->cursor++;
        }
        while (state->cursor < state->length && state->text[state->cursor] != ' ') {
            state->cursor++;
        }
    } else if (state->cursor < state->length) {
        state->cursor++;
    }
    state->preferred_col = column_for_index(state, state->cursor);
}

static void editor_move_home(EditorState* state) {
    state->cursor = line_start_for_index(state, state->cursor);
    state->preferred_col = 0U;
}

static void editor_move_end(EditorState* state) {
    uint32_t start = line_start_for_index(state, state->cursor);
    state->cursor = line_end_for_start(state, start);
    state->preferred_col = column_for_index(state, state->cursor);
}

static void editor_copy_selection(EditorState* state) {
    uint32_t start;
    uint32_t end;

    if (state->selection_anchor == 0xFFFFFFFFU || state->selection_anchor == state->cursor) {
        return;
    }

    start = state->selection_anchor < state->cursor ? state->selection_anchor : state->cursor;
    end = state->selection_anchor > state->cursor ? state->selection_anchor : state->cursor;
    clipboard_set(&state->text[start], end - start);
}

static void editor_paste_clipboard(EditorState* state) {
    uint32_t length = clipboard_length();
    const char* data = clipboard_data();

    if (state->selection_anchor != 0xFFFFFFFFU && state->selection_anchor != state->cursor) {
        editor_backspace(state);
    }

    for (uint32_t i = 0; i < length; i++) {
        editor_insert(state, data[i]);
    }
}

static void draw_title_bar(const EditorState* state) {
    console_set_color(COLOR_WHITE, COLOR_BLUE);
    console_clear_row(0);
    console_set_cursor(0, 0);
    console_write(" Prism Editor  ");
    console_write(state->path);
    if (state->modified) {
        console_write("  [modified]");
    }
}

static void draw_status_bar(uint32_t rows, uint32_t cursor_row, uint32_t cursor_col) {
    uint32_t line = cursor_row + 1U;
    uint32_t col = cursor_col + 1U;

    console_set_color(COLOR_BLACK, COLOR_LIGHT_GRAY);
    console_clear_row((int)(rows - 1U));
    console_set_cursor(0, (int)(rows - 1U));
    console_write("Esc menu  Arrows move  Enter new line  Backspace/Delete edit  ");
    console_write("Ln ");
    console_write_uint(line);
    console_write(" Col ");
    console_write_uint(col);
}

static uint32_t compute_row_signature(const EditorState* state, uint32_t line_start, uint32_t columns) {
    uint32_t pos = line_start;
    uint32_t col = 0;
    uint32_t hash = 2166136261U;

    hash ^= columns;
    hash *= 16777619U;

    while (pos < state->length && state->text[pos] != '\n' && col < columns) {
        hash ^= (uint8_t)state->text[pos];
        hash *= 16777619U;
        pos++;
        col++;
    }

    hash ^= col;
    hash *= 16777619U;
    return hash;
}

static void draw_text_area(EditorState* state, uint32_t columns, uint32_t rows, int force_redraw) {
    uint32_t visible_rows = rows - 2U;
    uint32_t line_start = line_start_for_row(state, state->scroll_row);

    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);

    for (uint32_t r = 0; r < visible_rows; r++) {
        uint32_t pos = line_start;
        uint32_t row_y = r + 1U;
        uint32_t col = 0;
        uint32_t signature = compute_row_signature(state, line_start, columns);
        int row_changed = force_redraw;

        if (r >= EDITOR_MAX_VISIBLE_ROWS) {
            row_changed = 1;
        } else if (!row_changed) {
            row_changed = (state->prev_row_valid[r] == 0U) || (state->prev_row_signature[r] != signature);
        }

        if (row_changed) {
            console_clear_row((int)row_y);
            console_set_cursor(0, (int)row_y);

            while (pos < state->length && state->text[pos] != '\n' && col < columns) {
                console_write_char(state->text[pos]);
                pos++;
                col++;
            }

            if (r < EDITOR_MAX_VISIBLE_ROWS) {
                state->prev_row_signature[r] = signature;
                state->prev_row_valid[r] = 1U;
            }
        }

        if (line_start >= state->length) {
            line_start = state->length;
        } else {
            while (line_start < state->length && state->text[line_start] != '\n') {
                line_start++;
            }

            if (line_start < state->length && state->text[line_start] == '\n') {
                line_start++;
            }
        }
    }

    if (state->prev_visible_rows > visible_rows) {
        uint32_t start = visible_rows;
        uint32_t end = state->prev_visible_rows;
        if (end > EDITOR_MAX_VISIBLE_ROWS) {
            end = EDITOR_MAX_VISIBLE_ROWS;
        }

        for (uint32_t r = start; r < end; r++) {
            state->prev_row_valid[r] = 0U;
        }
    }

    if (visible_rows > EDITOR_MAX_VISIBLE_ROWS) {
        state->prev_visible_rows = EDITOR_MAX_VISIBLE_ROWS;
    } else {
        state->prev_visible_rows = visible_rows;
    }
}

static void draw_selection(EditorState* state, uint32_t columns, uint32_t rows) {
    uint32_t start;
    uint32_t end;

    if (state->selection_anchor == 0xFFFFFFFFU || state->selection_anchor == state->cursor) {
        return;
    }

    start = state->selection_anchor < state->cursor ? state->selection_anchor : state->cursor;
    end = state->selection_anchor > state->cursor ? state->selection_anchor : state->cursor;
    console_set_color(COLOR_BLACK, COLOR_LIGHT_GRAY);
    for (uint32_t i = start; i < end; i++) {
        uint32_t row;
        uint32_t col;
        cursor_metrics_for_index(state, i, &row, &col);
        if (state->text[i] != '\n' && row >= state->scroll_row && row - state->scroll_row < rows - 2U && col < columns) {
            console_set_cursor((int)col, (int)(row - state->scroll_row + 1U));
            console_write_char(state->text[i]);
        }
    }
    console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
}

static void draw_editor(EditorState* state, int text_dirty) {
    uint32_t columns = editor_columns();
    uint32_t rows = editor_rows();
    uint32_t cursor_row;
    uint32_t cursor_col;
    int scroll_changed;
    int layout_changed;
    int needs_full_text;
    int force_row_redraw;

    if (columns == 0U || rows < 3U) {
        return;
    }

    cursor_metrics_for_index(state, state->cursor, &cursor_row, &cursor_col);
    scroll_changed = ensure_cursor_visible(state, cursor_row);

    layout_changed = (state->prev_columns != columns) || (state->prev_rows != rows);
    needs_full_text = state->full_redraw
        || text_dirty
        || scroll_changed
        || layout_changed
        || (state->prev_modified != state->modified)
        || (state->prev_length != state->length);
    force_row_redraw = state->full_redraw || scroll_changed || layout_changed;

    if (needs_full_text) {
        draw_title_bar(state);
        draw_text_area(state, columns, rows, force_row_redraw);
        state->full_redraw = 0;
        state->prev_modified = state->modified;
        state->prev_length = state->length;
        state->prev_scroll_row = state->scroll_row;
        state->prev_columns = columns;
        state->prev_rows = rows;
    }

    if (needs_full_text || state->prev_cursor_row != cursor_row || state->prev_cursor_col != cursor_col) {
        draw_status_bar(rows, cursor_row, cursor_col);
        state->prev_cursor_row = cursor_row;
        state->prev_cursor_col = cursor_col;
    }

    draw_selection(state, columns, rows);

    if (cursor_row >= state->scroll_row) {
        uint32_t local_row = cursor_row - state->scroll_row;
        if (local_row < (rows - 2U) && cursor_col < columns) {
            console_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
            console_set_cursor((int)cursor_col, (int)(local_row + 1U));
        }
    }
}

static int editor_save(EditorState* state) {
    if (vfs_write_file(state->path, state->text, state->length, 0) != 0) {
        ERROR_LOG("editor save failed");
        return -1;
    }

    DEBUG_LOG("editor save completed");
    state->modified = 0;
    return 0;
}

static void draw_exit_menu(uint32_t rows) {
    console_set_color(COLOR_YELLOW, COLOR_BLUE);
    console_clear_row((int)(rows - 1U));
    console_set_cursor(0, (int)(rows - 1U));
    console_write("Exit menu: S=save+exit  Q=quit  C=cancel");
}

static void handle_exit_prompt(EditorState* state) {
    uint32_t rows = editor_rows();

    while (1) {
        KeyEvent event;
        draw_exit_menu(rows);
        event = keyboard_read_event();

        if (event.type == KEY_EVENT_CHARACTER) {
            char c = event.character;
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }

            if (c == 'c') {
                return;
            }

            if (c == 'q') {
                DEBUG_LOG("editor exit without save");
                state->running = 0;
                return;
            }

            if (c == 's') {
                if (editor_save(state) == 0) {
                    DEBUG_LOG("editor save and exit");
                    state->running = 0;
                    return;
                }

                console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
                console_set_cursor(0, (int)(rows - 1U));
                console_clear_row((int)(rows - 1U));
                console_set_cursor(0, (int)(rows - 1U));
                console_write("Save failed. Press any key to continue editing.");
                (void)keyboard_read_event();
                return;
            }
        }
    }
}

int editor_app_run(const char* abs_path) {
    static EditorState state;
    uint32_t size = 0;

    DEBUG_LOG("editor app launch requested");

    if (abs_path == 0 || abs_path[0] != '/') {
        ERROR_LOG("editor app invalid path argument");
        return -1;
    }

    copy_limited(state.path, sizeof(state.path), abs_path);
    state.length = 0;
    state.cursor = 0;
    state.selection_anchor = 0xFFFFFFFFU;
    state.preferred_col = 0;
    state.scroll_row = 0;
    state.modified = 0;
    state.running = 1;
    state.prev_cursor_row = 0xFFFFFFFFU;
    state.prev_cursor_col = 0xFFFFFFFFU;
    state.prev_scroll_row = 0xFFFFFFFFU;
    state.prev_length = 0xFFFFFFFFU;
    state.prev_columns = 0xFFFFFFFFU;
    state.prev_rows = 0xFFFFFFFFU;
    state.prev_modified = -1;
    state.full_redraw = 1;
    state.prev_visible_rows = 0;
    for (uint32_t i = 0; i < EDITOR_MAX_VISIBLE_ROWS; i++) {
        state.prev_row_signature[i] = 0U;
        state.prev_row_valid[i] = 0U;
    }
    state.text[0] = '\0';

    if (vfs_read_file(abs_path, state.text, sizeof(state.text), &size) == 0) {
        state.length = size;
        state.cursor = size;
        state.text[state.length] = '\0';
        DEBUG_LOG("editor opened existing file");
    } else {
        DEBUG_LOG("editor starting with empty buffer");
    }

    {
        int text_dirty = 1;

        while (state.running) {
        KeyEvent event;

        draw_editor(&state, text_dirty);
        text_dirty = 0;
        event = keyboard_read_event();

        if (event.type == KEY_EVENT_CHARACTER) {
            if (event.character == 27) {
                DEBUG_LOG("editor exit menu opened");
                handle_exit_prompt(&state);
                state.full_redraw = 1;
                text_dirty = 1;
            } else if (event.character == '\t') {
                editor_insert(&state, ' ');
                editor_insert(&state, ' ');
                editor_insert(&state, ' ');
                editor_insert(&state, ' ');
                state.preferred_col = column_for_index(&state, state.cursor);
                text_dirty = 1;
            } else if (event.character >= 32 && event.character <= 126) {
                editor_insert(&state, event.character);
                state.preferred_col = column_for_index(&state, state.cursor);
                text_dirty = 1;
            }
            continue;
        }

        switch (event.type) {
            case KEY_EVENT_ENTER:
                editor_insert(&state, '\n');
                state.preferred_col = column_for_index(&state, state.cursor);
                text_dirty = 1;
                break;
            case KEY_EVENT_BACKSPACE:
                editor_backspace(&state);
                state.preferred_col = column_for_index(&state, state.cursor);
                text_dirty = 1;
                break;
            case KEY_EVENT_DELETE:
                editor_delete(&state);
                state.preferred_col = column_for_index(&state, state.cursor);
                text_dirty = 1;
                break;
            case KEY_EVENT_LEFT:
                if ((event.modifiers & KEY_MOD_SHIFT) != 0U && state.selection_anchor == 0xFFFFFFFFU) {
                    state.selection_anchor = state.cursor;
                } else if ((event.modifiers & KEY_MOD_SHIFT) == 0U) {
                    state.selection_anchor = 0xFFFFFFFFU;
                }
                editor_move_left(&state, (event.modifiers & KEY_MOD_CTRL) != 0U);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_RIGHT:
                if ((event.modifiers & KEY_MOD_SHIFT) != 0U && state.selection_anchor == 0xFFFFFFFFU) {
                    state.selection_anchor = state.cursor;
                } else if ((event.modifiers & KEY_MOD_SHIFT) == 0U) {
                    state.selection_anchor = 0xFFFFFFFFU;
                }
                editor_move_right(&state, (event.modifiers & KEY_MOD_CTRL) != 0U);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_UP:
                state.selection_anchor = 0xFFFFFFFFU;
                editor_move_vertical(&state, -1);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_DOWN:
                state.selection_anchor = 0xFFFFFFFFU;
                editor_move_vertical(&state, 1);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_HOME:
                state.selection_anchor = 0xFFFFFFFFU;
                editor_move_home(&state);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_END:
                state.selection_anchor = 0xFFFFFFFFU;
                editor_move_end(&state);
                state.full_redraw = 1;
                break;
            case KEY_EVENT_COPY:
                editor_copy_selection(&state);
                break;
            case KEY_EVENT_PASTE:
                editor_paste_clipboard(&state);
                state.full_redraw = 1;
                text_dirty = 1;
                break;
            case KEY_EVENT_NONE:
            default:
                break;
        }
    }
    }

    console_set_color(COLOR_WHITE, COLOR_BLACK);
    console_clear();
    DEBUG_LOG("editor app exited");
    return 0;
}
