# PrismOS

PrismOS is a small hobby operating system that boots via GRUB and runs on x86 hardware (or in QEMU). It provides a simple framebuffer console, a basic shell, and a handful of built-in commands.

## Quick build & run

Install the build dependencies (example for Debian/Ubuntu):

```shell
sudo apt-get install build-essential nasm gcc-multilib xorriso qemu-system-x86 qemu-utils grub-common grub-pc-bin mtools dosfstools util-linux
```

Build and run:

```shell
make run           # build (if needed) and boot the ISO in QEMU
```

Other useful targets:

```shell
make clean         # remove build artifacts
make run-serial    # boot and mirror COM1 to your terminal
make run-serial-log # boot and save COM1 output to build/serial.log
make run-vhdx      # convert the test disk to VHDX and boot it in QEMU
make run-secondary # attach a separate 2 GiB FAT32 IDE disk for drive-switch testing
```

Clipboard sharing:
- Use `make run-serial` when launching PrismOS from a terminal. Pasting into that terminal sends host text to the guest through COM1.
- `Ctrl+C` on a selection publishes the guest text with OSC 52; terminals that support OSC 52 can place it on the host clipboard.
- The regular QEMU graphical window does not provide native clipboard sharing yet. That requires a guest-side SPICE/virtio clipboard protocol, which PrismOS does not currently implement.

Notes:
- The build uses 32-bit compilation flags (gcc -m32). Ensure you have multilib support installed.
- `make run` launches QEMU for quick testing; you can also boot `build/os.iso` in a BIOS-mode VM.
- `make run-serial` launches QEMU with debug logs. If you are running bare-metal use serial port.

If you want a reproducible cross-toolchain build, replace the host `gcc` invocations with an i686-elf cross-compiler and adjust the Makefile accordingly.

## Disk and partition support

PrismOS probes up to four ATA hard drives on the primary and secondary legacy
IDE channels, scans MBR primary partitions, and mounts the first valid FAT32
partition (preferring an active partition). Legacy FAT32 volumes that occupy
the whole disk are also accepted. Use `drive` to list detected drives and their
capacities, `partitions` to open the full-screen disk manager, and
`drive <index>` to switch the physical ATA drive. Drive indices are not
partition slots. In the manager, use Up/Down to select a slot, M to mount a
formatted FAT32 partition, C to create, S to shrink the mounted FAT32 partition,
D to delete (with confirmation), F to format a FAT32 primary partition
(destructive, with confirmation), R to relabel the mounted FAT32 volume, and Esc
to exit.
Shrink only reclaims an unallocated tail; move/remove data there first. Use C
afterward to create a partition in the reclaimed space. `partitions list` prints
a text overview. Shell operations are also available as `partitions mount
<slot>`, `partitions create <sizeMiB>`, `partitions shrink <slot> <reduceByMiB>`,
`partitions delete <slot>`, and `partitions rename <slot> <label>`. Creation adds an aligned FAT32-type
MBR entry but does not format it; use F in the manager to quick-format a FAT32
primary partition with the `PRISMOS` label (all contents are erased). Deletion
removes only an unmounted MBR entry and does not erase its sectors; rename
changes the currently mounted FAT32 volume label. Switching drives resets the
shell working directory to `/`; use
`cd` to navigate that drive's filesystem.

Disk discovery is non-destructive: boot no longer formats an unrecognized or
blank drive. Prepare a FAT32 partition before booting PrismOS, and back up data
before testing on physical hardware. This initial implementation supports MBR
and ATA LBA28 addressing; GPT, extended/logical partitions, SATA/AHCI, and
drives larger than the LBA28 addressable range are not yet supported. The QEMU
test image is created as an MBR disk with a FAT32 partition; whole-disk FAT32
images remain supported for compatibility.

QEMU can use VHDX containers without any VHDX parsing in the kernel: QEMU
exposes the container as the same emulated IDE disk. `make run-vhdx` converts
the generated raw test disk into `build/disk.vhdx` and boots from it. To attach
an existing VHDX instead, set `QEMU_DISK_IMAGE` and `QEMU_DISK_FORMAT`, for
example `make run QEMU_DISK_IMAGE=/path/to/disk.vhdx QEMU_DISK_FORMAT=vhdx`.
Use a disposable image for testing because PrismOS writes to mounted FAT32
volumes.

`make run-secondary` creates a sparse `build/disk-secondary.img` with a 2 GiB
MBR/FAT32 volume labeled `SECONDARY` and attaches it as the second IDE drive.
In PrismOS, run `drive` to list physical drive indices, then `drive 1` to switch
to the secondary disk and confirm the prompt shows `SECONDARY:/`. Both test
images are writable and their contents persist between runs.

To edit files from the host before booting, mount the image, edit files under
`build/disk-mnt`, then unmount it before starting QEMU:

```shell
make mount-disk       # mounts the default raw image
# edit/copy files under build/disk-mnt
make unmount-disk
make run
```

For the VHDX image, use `make mount-vhdx` and `make unmount-vhdx`, then boot
with `make run-vhdx`. These mount targets use `qemu-nbd` and `sudo`; the host
needs NBD and FAT filesystem support. Never run PrismOS on the image while it
is mounted on the host, and unmount it cleanly first so changes are flushed.
QEMU run targets and `make clean` refuse to proceed while the default host mount
is active, to help prevent simultaneous access or accidental data deletion.

## Command help

Run `help` for the full-screen, alphabetized command browser. Use Up/Down to
select commands, Home/End to jump through the list, type a letter to jump to a
command, and Enter to open its detailed description, syntax, parameter notes,
and example. Esc returns from details or closes the browser; Q closes it
immediately. Use `help <command>` for the same information as a short shell
summary.


# Developing and Contributing to PrismOS
> [!IMPORTANT]
> All code you commit must be clean and have comments.
> For development, edit sources under `src/`, then re-run `make run`.

## Subset C Compiler (prismcc)

PrismOS includes a lightweight subset-C pipeline that targets a bytecode VM inside the OS.

Build the compiler:

```shell
make prismcc
```

Compile a program to a Prism app package:

```shell
cat > hello.c <<'EOF'
int main() {
		int a = 2 + 3;
		print("Hello from prismcc");
		print_int(a * 10);
		return 0;
}
EOF

./build/prismcc hello.c hello.app
```

Copy the generated app to PrismOS disk content (8.3 FAT name recommended), then run in PrismOS shell:

```text
app-run /HELLO.APP
```

### Supported Subset

- One function: `int main() { ... }`
- Statements:
	- `int x = expr;`
	- `x = expr;`
	- `print("text");`
	- `print_int(expr);`
	- `return expr;`
- Expressions: integer literals, variables, `+ - * /`, unary `-`, parentheses

The compiler emits Prism app containers with a BCVM bytecode payload, and PrismOS runs them through the in-kernel bytecode VM.

## Native Compiler Inside PrismOS

PrismOS now includes an in-OS subset C compiler command:

```text
cc <input.c> <output.app|output.pdr>
```

Example flow from PrismOS shell:

```text
edit /HELLO.C
cc /HELLO.C /HELLO.APP
app-run /HELLO.APP
```

This uses the same subset language as `prismcc` and compiles directly on the PrismOS filesystem.

### PrismCC driver modules

The in-OS compiler can also create cooperative PrismCC driver packages. A driver source defines `int driver_init()`, `void driver_poll()`, `void driver_shutdown()`, and `int main()`. Its `main()` calls initialization, polls until `driver_should_stop()` observes Esc, then shuts down.

```text
mkdir /DRIVERS
cc /SERDRV.C /DRIVERS/SERIAL.PDR
driver-run /DRIVERS/SERIAL.PDR
```

Driver-only built-ins provide COM1 serial read/write, allowlisted COM1 port I/O (`0x3F8..0x3FF`), and checked 32-bit MMIO access to regions explicitly registered by trusted kernel code. The PrismOS FAT32 volume uses 8.3 names, so driver packages use the `.PDR` suffix. See [PRISMCC_COMPILER_FEATURES.md](PRISMCC_COMPILER_FEATURES.md) and [examples/serdrv.c](examples/serdrv.c) for the ABI and sample; copy the source to `/SERDRV.C` first. Drivers execute in the kernel's shared address space without process isolation; load only trusted code. The standalone `make prismcc` host compiler remains the small app-only compiler; compile driver packages with PrismOS's `cc` command.

## Built-in Banking App

PrismOS now auto-installs a built-in banking app package at boot:

- `/APPS/BANK.APP`

Run it from the shell with:

```text
app-run /APPS/BANK.APP
```

The app stores accounts locally in:

- `/DATA/BANK/ACCOUNTS.DB`

Each account record contains username, password, and balance, and supports login, deposit, and withdrawal.

## Program Data Folder

PrismOS now creates a dedicated program-data folder during boot:

- `/DATA`
- `/DATA/APPS`

Application code should store app data under `/DATA` instead of mixing data with app binaries under `/APPS`.

For PrismCC apps, you can use the new stdlib header:

- `#include "std/prism_data.h"`

This header provides wrappers for filesystem operations oriented around app data paths.
