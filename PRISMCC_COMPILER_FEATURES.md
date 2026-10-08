# PrismCC Compiler Features

This document describes the current feature set of the in-OS PrismCC compiler and BCVM runtime.

## Overview

- Compiler: `src/apps/prismcc_runtime.c`
- VM runtime: `src/apps/bytecode_vm.c`
- Bytecode format/opcodes: `src/apps/app_format.h`
- Shell integration command: `cc <input.c> <output.app>`

PrismCC compiles a C-like subset into BCVM bytecode packaged in the Prism app format.

## Language: Supported Types

- `int`
- `int8`, `int16`, `int32`
- `uint8`, `uint16`, `uint32`
- `char` (int-backed single-character value)
- `string`
- `float` (fixed-point with 3 decimal places)
- `void` (function return type)
- `int[N]` fixed-size arrays
- `struct` types with integer/string/float fields

Notes:
- Integer aliases currently map to VM 32-bit integer behavior.
- Floats use fixed-point scaling (`1000`).
- String values are first-class VM values (descriptors).
- Local variable limit per function: 64.

## Language: Functions and Calls

- User-defined functions are supported.
- Function parameters are supported (`int` and `string`).
- Function parameters are supported (`int`/integer aliases, `char`, `string`, `float`).
- Return values are supported.
- `void` methods are supported (`void name(...) { ... }`).
- Method overloading is supported by signature:
  - same name
  - different parameter type list
- Overload resolution is performed during compile/patch stage.

## Preprocessor and Includes

- Supports textual includes via:
  - `#include "relative/path.h"`
  - `#include <relative/or/absolute/path.h>`
- Include paths are resolved relative to the including file when not absolute.
- Header files (`.h`) are supported as normal include targets.
- Includes are expanded before lexing/parsing (single translation unit).
- Cyclic includes are rejected.

## Language: Statements and Control Flow

- Variable declarations:
  - `int x;`
  - `int16 x;`
  - `uint32 x;`
  - `int x = 5;`
  - `float f = 1.250;`
  - `char c;`
  - `char c = 'A';`
  - `string s;`
  - `string s = "hi";`
  - `int a[8];`
  - `float samples[8];`
  - `string lines[8];`
  - `struct Person people[4];`
  - `struct Person p;`
- Assignment:
  - scalar assignment (`x = expr;`)
  - array element assignment (`a[i] = expr;`)
  - struct field assignment (`p.name = "neo";`, `p.age = 7;`)
- `if` / `else`
- `while`
- `for (init; condition; increment)`
- `return expr;` for non-void methods
- `return;` for void methods

## Language: Expressions and Operators

- Integer arithmetic:
  - `+`, `-`, `*`, `/`, `%`
  - unary `+`, unary `-`
  - `++a`, `--a`, `a++`, `a--` for `int` locals
- Float arithmetic:
  - `+`, `-`, `*`, `/`
  - `*` and `/` use fixed-point VM opcodes
  - `%` is not supported for float
  - mixing int and float in a single arithmetic operation is not supported implicitly
- Integer comparisons:
  - `==`, `!=`, `<`, `<=`, `>`, `>=`
- Function call expressions
- Parenthesized expressions
- String literals
- Char literals with single quotes (`'A'`, `'\n'`, `'\''`)
- Array element reads (`a[i]`)
- Struct field reads (`p.name`, `p.age`)

## Structs

- Type definition syntax:
  - `struct Person { int age; char initial; string name; };`
- Field types currently supported in structs:
  - `int`
  - `char`
  - `string`
- Struct variables are value containers backed by VM heap arrays.
- Struct initializers are not supported yet (declare first, then assign fields).

## Built-ins: Console and Input

- `print(expr)`
  - overloaded behavior:
    - prints int when expression is int
    - prints string when expression is string
  - prints newline after output
- `print_int(expr)`
- `print_char(expr)`
  - prints one character from the low byte of int expression
  - does not print newline
- `print_color(colorExpr, stringExpr)`
- `read_text()`
- `read_text(promptStringExpr)`
- `input_int()`
- `print_input()`
- `input_len()`
- `input_eq("literal")`

## Built-ins: String

- `string_len(stringExpr)`
- `string_eq(stringExprA, stringExprB)`

## Built-ins: Filesystem

- `file_read(pathStringExpr)` -> returns string
- `file_write(pathStringExpr, textStringExpr);` -> status is produced and discarded in statement form
- `file_append(pathStringExpr, textStringExpr);` -> status is produced and discarded in statement form
- `file_exists(pathStringExpr)` -> returns int (`1` exists, `0` does not exist)

Path strings are passed directly to VFS operations in the VM.

Program data convention:
- Store app data files under `/DATA` (or `/DATA/APPS` for grouped app data).

## PrismCC Driver Modules

The in-OS compiler can build a cooperative driver package when the output filename ends in `.pdr` (`.pdrv` is also recognized on filesystems that support four-character extensions). The PrismOS FAT32 implementation uses 8.3 names, so `.pdr` is the supported on-disk suffix:

```text
mkdir /DRIVERS
cc /SERDRV.C /DRIVERS/SERIAL.PDR
driver-run /DRIVERS/SERIAL.PDR
```

Driver sources must define zero-argument `int driver_init()`, `void driver_poll()`, `void driver_shutdown()`, and `int main()` functions. `main()` owns the lifecycle: initialize, poll until `driver_should_stop()` returns nonzero, and shut down. Press Esc to request a stop. The compiler validates these entry points and sets the driver package flag; `app-run` refuses driver packages.

Driver hardware built-ins are compiled into BCVM host calls and are enabled only by `driver-run`:

- `driver_serial_write(string)` -> returns `0` on success, `-1` on invalid context/input.
- `driver_serial_read()` -> reads one queued COM1 byte, or `-1` if none is available.
- `driver_io_read8(port)` -> reads a byte from the allowlisted COM1 I/O range `0x3F8..0x3FF`; returns `-1` outside it.
- `driver_io_write8(port, value)` -> writes one byte in the COM1 range; returns `0` or `-1`.
- `driver_mmio_read32(address)` / `driver_mmio_write32(address, value)` -> access aligned 32-bit addresses only inside MMIO ranges registered by trusted kernel code through `driver_mmio_register_region()`.
- `driver_should_stop()` -> nonblocking check for the Esc stop request.

The C driver API in `src/drivers/driver_api.h` is the kernel-side extension point for adding device-specific port windows, MMIO mappings, and services. The PrismCC MMIO operations do not accept arbitrary addresses; a kernel driver must register a region first. PrismCC drivers run in the kernel's shared address space with no ring-3 isolation, so only trusted code should be loaded. They must not execute blocking loops in interrupt context; bytecode drivers are cooperative and are never called from an ISR.

The COM1 example at `examples/serdrv.c` polls the UART line-status/data registers and echoes received bytes. It temporarily disables UART receive IRQs to avoid competing with the kernel's UART IRQ handler and restores them in `driver_shutdown()`. Place a copy at `/SERDRV.C` on the PrismOS volume before compiling.

The standalone host tool `make prismcc` remains the minimal one-function app compiler. `.pdr`/`.pdrv` modules and driver host calls are supported by the in-OS compiler command `cc`.

## Arrays

- Fixed-size declaration with constant positive size (`int a[16];`, `float a[16];`, `string a[16];`, `struct T a[16];`)
- Zero-initialized when created
- Indexed read/write:
  - `x = a[i];`
  - `a[i] = x;`

Runtime behavior:
- Array storage is VM heap backed (handle-based)
- Out-of-range access fails VM execution

## Compiler and Runtime Limits

Compiler-side limits:
- Source size: 32 KiB
- Code size: 48 KiB
- Data size: 16 KiB
- Max locals: 512
- Max functions: 64
- Max call patch sites: 256

VM-side limits:
- Stack entries: 256
- Call depth: 32
- Max VM steps per run: 200000
- Input buffer: 127 chars
- Heap string slots: 64 (data pool 4096 bytes)
- Heap array slots: 64 (cell pool 1024 int cells)

## Shell Workflow

1. Write source file (for example under `/examples`).
  - You can split reusable code into headers and include them.
2. Compile in PrismOS shell:
   - `cc /path/program.c /path/program.app`
3. Run app:
   - `app-run /path/program.app`

### Small Standard Library (Headers)

The repository now includes a small header-only standard library under `/examples/std`:

- `/examples/std/prism_string.h`
  - `std_str_len`, `std_str_eq`, emptiness and length checks
- `/examples/std/prism_math.h`
  - arithmetic wrappers, `std_abs`, `std_min`, `std_max`, `std_clamp`, `std_pow2`
- `/examples/std/prism_io.h`
  - print/read helpers and filesystem wrappers
- `/examples/std/prism_data.h`
  - app-data oriented wrappers and `/DATA` path helpers

Usage example:

- `#include "std/prism_string.h"`
- `#include "std/prism_math.h"`
- `#include "std/prism_io.h"`
- `#include "std/prism_data.h"`

## Current Known Gaps

- No unions/enums
- No pointers
- No native IEEE-754 floating-point backend (current float support is fixed-point)
- No global variables
- No module/import system
- No separate object/link pipeline yet (includes are textual expansion)
- No dynamic array type beyond fixed `int[N]`
- No struct fields of type array or nested struct yet
- No string interpolation/format library yet
- Filesystem built-ins currently use simple status returns; richer error model is pending
