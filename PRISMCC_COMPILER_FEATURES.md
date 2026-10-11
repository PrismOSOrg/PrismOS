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

- Integer bitwise operators: `&`, `|`, `^`, `~`, `<<`, `>>`
- Hexadecimal integer literals: `0x...` / `0X...`
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
- `read_key()` -> blocks for one key and returns its character, newline, backspace, or `-1` for an unsupported key; the integer result can be used directly in conditions
- Built-in key constants include `KEY_A` through `KEY_Z`, `KEY_0` through `KEY_9`, `KEY_ENTER`, `KEY_ESCAPE`, `KEY_BACKSPACE`, `KEY_TAB`, and `KEY_SPACE`.
- `read_key()` -> blocks for one key and returns its character, newline, backspace, or `-1` for an unsupported key
- `app_wait_key_release(key)` -> blocks until a physical key is released, yielding time to resident drivers while it waits; returns `0` if Escape was pressed instead
- `driver_service_call(driverId, operation, argument0, argument1)` -> synchronously calls a resident PrismCC driver's exported service and returns its integer result, or `-1` if unavailable/failed
- `driver_service_find(serviceType)` -> returns the first running resident driver that accepts service-discovery operation `0` with the given service type, or `-1` if none is available
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

The in-OS compiler can build a driver package when the output filename ends in `.pdr` (`.pdrv` is also recognized on filesystems that support four-character extensions). The PrismOS FAT32 implementation uses 8.3 names, so `.pdr` is the supported on-disk suffix:

```text
mkdir /DRIVERS
cc /SERDRV.C /DRIVERS/SERIAL.PDR
driver-start /DRIVERS/SERIAL.PDR
driver-list
driver-stop 1
```

Driver sources must define zero-argument `int driver_init()`, `void driver_poll()`, `void driver_shutdown()`, and `int main()` functions. `driver-start` runs the three lifecycle callbacks as a resident service; `driver-list` shows active IDs and states; `driver-stop <id>` requests shutdown. A resident driver that polls `driver_should_stop()` is also shut down when Esc is pressed; the check leaves non-Escape keyboard events queued for the shell or foreground app. `driver-run` remains a synchronous foreground mode in which `main()` owns the lifecycle. The compiler validates the entry points and sets the driver package flag; driver packages cannot be launched as applications.

Any resident driver may additionally export `int driver_service(int operation, int argument0, int argument1)`. Apps call it with `driver_service_call(driverId, operation, argument0, argument1)`; the driver ID is the one shown by `driver-list`. For discovery, a driver should return `1` for operation `0` when `argument0` matches one of its service-type identifiers, and `0` otherwise. Apps use `driver_service_find(serviceType)` to find the first matching running driver. The callback runs synchronously in that driver's isolated VM context and returns an integer to the app. A driver without this optional callback remains usable through its lifecycle but has no app-callable service. Legacy packages with 16-byte lifecycle tables remain loadable; recompile a package to include the optional service entry. Each callback is bounded to 256 VM slices, and a callback that exceeds that limit is stopped.

Driver hardware built-ins are compiled into BCVM operations and run by the ring-3 interpreter. Hardware access is available only through the kernel's validated system-call bridge:

- `driver_serial_write(string)` -> returns `0` on success, `-1` on invalid context/input.
- `driver_serial_read()` -> reads one queued COM1 byte, or `-1` if none is available.
- `driver_io_read8(port)` -> reads a byte from the allowlisted COM1 I/O range `0x3F8..0x3FF`; returns `-1` outside it.
- `driver_io_write8(port, value)` -> writes one byte in the COM1 range; returns `0` or `-1`.
- `driver_mmio_read32(address)` / `driver_mmio_write32(address, value)` -> access aligned 32-bit addresses only inside MMIO ranges registered by trusted kernel code through `driver_mmio_register_region()`.
- `driver_pci_find(vendorId, deviceId)` -> finds the first matching PCI function and returns its BDF encoded as `(bus << 16) | (device << 8) | function`, or `-1`.
- `driver_pci_read_config32(bdf, offset)` -> reads an aligned 32-bit PCI configuration register in the conventional 256-byte configuration space; returns `-1` for invalid or missing devices/offsets.
- `driver_pci_read_bar32(bdf, index)` -> returns a BAR's 32-bit base address, or `-1` if unavailable or above the 32-bit address space.
- `driver_pci_claim(bdf)` -> exclusively claims one PCI function for the current driver instance; returns `0` or `-1`.
- `driver_pci_enable(bdf, flags)` / `driver_pci_disable(bdf, flags)` -> enable or disable PCI I/O, memory decode, and bus mastering. Flags are `1`, `2`, and `4` respectively and may be combined with bitwise OR. The BDF must be claimed by this driver.
- `driver_pci_claim_io_bar(bdf, barIndex)` -> claims and sizes an I/O BAR on the claimed function, returning an opaque region handle or `-1`. I/O decode must be enabled and bus mastering must be disabled during BAR sizing.
- `driver_pci_io_read(handle, offset, width)` / `driver_pci_io_write(handle, offset, width, value)` -> perform aligned 8-, 16-, or 32-bit port access inside the claimed BAR's probed range.
- `driver_pci_claim_mmio_bar(bdf, barIndex)` -> claims and maps a 32-bit memory BAR up to 8 MiB, returning an opaque region handle or `-1`. Memory decode must be enabled and bus mastering must be disabled during BAR sizing. Mappings are uncached and are removed when the driver exits.
- `driver_pci_mmio_read32(handle, offset)` / `driver_pci_mmio_write32(handle, offset, value)` -> perform aligned 32-bit MMIO within a claimed, mapped BAR.
- `driver_pci_subscribe_irq(bdf)` -> subscribe to the claimed PCI function's legacy PIC interrupt line and return its IRQ number, or `-1` if the function has no usable line.
- `driver_irq_read()` -> dequeue one pending IRQ number, or `-1` if no event is queued.
- `driver_irq_dropped()` -> report IRQ notifications coalesced while waiting for acknowledgement or dropped because the bounded queue filled.
- `driver_irq_ack()` -> re-enable the subscribed PIC line after the driver has serviced the device interrupt and drained its queued events.
- `driver_dma_alloc(size)` -> allocates a zeroed, contiguous DMA buffer of at most 64 KiB below 4 GiB, returning an opaque handle or `-1`. Physical addresses are available through `driver_dma_phys(handle)` as a 32-bit bit pattern in an `int` (values at or above 2 GiB appear negative); compare the failure sentinel using `== -1`.
- `driver_dma_alloc_below(size, maxAddressExclusive)` -> allocates below a device-specific 32-bit DMA address limit.
- `driver_dma_read(handle, offset, width)` / `driver_dma_write(handle, offset, width, value)` -> access aligned 8-, 16-, or 32-bit values within an allocated DMA buffer.
- `driver_dma_free(handle)` -> frees a DMA buffer only while PCI bus mastering is disabled.
- `driver_should_stop()` -> nonblocking check for the Esc stop request; it does not consume other keyboard input.
- `driver_state_read(slot)` / `driver_state_write(slot, value)` -> read or update one of 32 persistent per-driver integer slots, useful across resident callbacks.

Each driver instance may claim one PCI device and at most eight BAR I/O regions, eight BAR MMIO regions, and eight DMA buffers. BAR I/O and MMIO handles are bounds-checked; I/O regions cannot overlap COM1. DMA buffers map into a dedicated virtual window and may reside below the requested physical address limit. The runtime disables bus mastering before reclaiming DMA memory, removes dynamic MMIO mappings, restores PCI command bits, and releases the device after normal exit or a contained user-mode fault. Drivers execute with user page permissions and cannot directly access supervisor memory or issue port I/O instructions; only the documented syscall services are available. BCVM packages containing app-only input, filesystem, or UI opcodes are rejected by the driver loader.

The AC'97 sample in `examples/ac97_prismcc.c` advertises audio service type `0x41554449` (`AUDI`), exports operation `1` to start a continuous tone using `argument0` as frequency in hertz, and operation `2` to stop playback. It requires an AC'97 codec with variable-rate audio support. A precomputed sine wave is repeated through a long DMA descriptor chain, and the resident driver restarts the chain if it reaches its end. `examples/piano_prismcc.c` discovers the audio service automatically and holds each tone until its physical key is released.

For a one-command QEMU setup, run `make run-piano`. It rebuilds the ISO, recreates the dedicated `build/piano-disk.img` without touching the normal disk image, copies both sources onto it, and automatically compiles and launches the piano with the AC'97 device attached. The audio backend defaults to silent; set `QEMU_AUDIO_BACKEND=alsa` or `QEMU_AUDIO_BACKEND=pa` if that host backend is available.

To use the piano on an existing PrismOS volume, copy the two sources as `/AC97.C` and `/PIANO.C`, then compile and run them from the OS shell:

```text
cc /AC97.C /DRIVERS/AC97.PDR
driver-start /DRIVERS/AC97.PDR
driver-list
cc /PIANO.C /APPS/PIANO.APP
/APPS/PIANO.APP
```

The piano automatically discovers the running audio driver. Play with `Z X C V B N M ,` (white keys) and `S D G H J` (black keys), and press Escape to exit. Stop the driver afterward with `driver-stop <id>`.

Resident drivers are scheduled round-robin from the 100 Hz PIT tick, one driver slice per tick. A timer interrupt requests a yield, which the isolated BCVM checks at most every 128 bytecode instructions; slices also have a 5,000-instruction ceiling. This is preemptive time-slicing at BCVM instruction boundaries on a single CPU, not parallel execution. The foreground `driver-run` command remains synchronous.

Interrupt delivery is limited to claimed PCI devices using an eligible legacy 8259 PIC IRQ line. IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade), IRQ4 (COM1), IRQ8 (RTC), and lines already owned by kernel handlers cannot be claimed. The IRQ handler only queues an event; it never runs driver code. After subscribing with `driver_pci_subscribe_irq(bdf)`, the driver reads events in `driver_poll()` with `driver_irq_read()`, acknowledges the device through its BAR, drains the queue, then calls `driver_irq_ack()` to re-enable the PIC line. The line remains masked until acknowledged to prevent an unserviced device from causing an interrupt storm. MSI/MSI-X and APIC interrupt routing are not supported.

Existing packages should be recompiled with the in-OS compiler to include resident lifecycle metadata and the new interrupt built-ins.

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
   - `/path/program.app`

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
