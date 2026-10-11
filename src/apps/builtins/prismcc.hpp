#ifndef PRISMOS_APPS_BUILTINS_PRISMCC_HPP
#define PRISMOS_APPS_BUILTINS_PRISMCC_HPP

/*
 * The built-in applications are kernel-linked C++ programs, while the
 * services they use remain C ABI kernel services.  This is the small
 * freestanding facade shared by every C++ built-in.
 */
extern "C" {
#include "apps/prismcc_runtime.h"
#include "debug/log.h"
#include "display/console.h"
#include "filesystem/vfs.h"
#include "input/keyboard.h"
#include "util/clipboard.h"
#include "util/string.h"
}

namespace PrismCC {
using ::KeyEvent;
using ::KeyEventType;
using ::VGA_Color;
using ::vfs_entry_t;

using ::clipboard_clear;
using ::clipboard_data;
using ::clipboard_length;
using ::clipboard_set;
using ::console_clear;
using ::console_clear_row;
using ::console_draw_pixel;
using ::console_get_framebuffer_height;
using ::console_get_framebuffer_width;
using ::console_get_x;
using ::console_get_y;
using ::console_set_bg_color;
using ::console_set_color;
using ::console_set_cursor;
using ::console_set_cursor_visible;
using ::console_set_fg_color;
using ::console_write;
using ::console_write_char;
using ::console_write_uint;
using ::console_writeln;
using ::keyboard_key_pressed;
using ::keyboard_peek_event;
using ::keyboard_poll_event;
using ::keyboard_read_event;
using ::memcpy;
using ::memset;
using ::prismcc_compile_file;
using ::strcmp;
using ::strlen;
using ::strncmp;
using ::strcpy;
using ::vfs_get_space;
using ::vfs_get_volume_label;
using ::vfs_init;
using ::vfs_list;
using ::vfs_mkdir;
using ::vfs_normalize_path;
using ::vfs_path_is_dir;
using ::vfs_read_file;
using ::vfs_rm;
using ::vfs_rmdir;
using ::vfs_set_volume_label;
using ::vfs_touch;
using ::vfs_unmount;
using ::vfs_write_file;
}

#endif
