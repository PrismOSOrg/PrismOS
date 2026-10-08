CC=gcc
LD=gcc

CPPFLAGS=-Isrc
CFLAGS=-m32 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pic -fno-pie
LDFLAGS=-m32 -T boot/linker.ld -ffreestanding -nostdlib -no-pie -Wl,--build-id=none

BUILD=build
ISO_DIR=$(BUILD)/isodir
DISK_MOUNT_POINT ?= $(BUILD)/disk-mnt

QEMU=qemu-system-x86_64
QEMU_DISK_IMAGE ?= $(BUILD)/disk.img
QEMU_DISK_FORMAT ?= raw
QEMU_FLAGS=-boot order=d -cdrom $(BUILD)/os.iso \
	-drive file=$(QEMU_DISK_IMAGE),format=$(QEMU_DISK_FORMAT),if=ide,index=0,media=disk \
	-monitor none

.PHONY: all run run-serial run-serial-log run-vhdx run-secondary run-net run-net-serial check-disk-unmounted mount-disk unmount-disk mount-vhdx unmount-vhdx prismcc clean

all: os.iso

$(BUILD):
	mkdir -p $(BUILD)

# -------------------------
# Core objects
# -------------------------
CORE_OBJS = \
$(BUILD)/boot.o \
$(BUILD)/kernel.o \
$(BUILD)/console.o \
$(BUILD)/psf_font.o \
$(BUILD)/keyboard.o \
$(BUILD)/serial.o \
$(BUILD)/comport.o \
$(BUILD)/log.o \
$(BUILD)/shell.o \
$(BUILD)/command.o \
$(BUILD)/font_psf.o \
$(BUILD)/string.o \
$(BUILD)/clipboard.o

# -------------------------
# Systems
# -------------------------
INTERRUPT_OBJS = \
$(BUILD)/gdt.o \
$(BUILD)/pic.o \
$(BUILD)/isr_stubs.o \
$(BUILD)/idt.o \
$(BUILD)/irq.o \
$(BUILD)/interrupts.o \
$(BUILD)/ringbuf.o

MEMORY_OBJS = \
$(BUILD)/pmm.o \
$(BUILD)/paging.o

DRIVER_OBJS = \
$(BUILD)/driver_api.o \
$(BUILD)/pci.o

NETWORK_OBJS = \
$(BUILD)/network.o \
$(BUILD)/ethernet.o \
$(BUILD)/arp.o \
$(BUILD)/ipv4.o \
$(BUILD)/udp.o \
$(BUILD)/dhcp.o \
$(BUILD)/icmp.o \
$(BUILD)/tcp.o \
$(BUILD)/e1000.o

FS_OBJS = \
$(BUILD)/blockdev.o \
$(BUILD)/partition_manager.o \
$(BUILD)/fat32.o \
$(BUILD)/vfs.o

APP_OBJS = \
$(BUILD)/app_loader.o \
$(BUILD)/app_runtime.o \
$(BUILD)/bytecode_vm.o \
$(BUILD)/prismcc_runtime.o \
$(BUILD)/help_app.o \
$(BUILD)/partition_manager_app.o \
$(BUILD)/editor_app.o \
$(BUILD)/ide_app.o \
$(BUILD)/bank_app.o \
$(BUILD)/app_manager.o

# -------------------------
# Compile rules
# -------------------------
$(BUILD)/%.o: boot/%.s | $(BUILD)
	gcc -m32 -c $< -o $@

$(BUILD)/kernel.o: src/kernel.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/debug/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/display/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/input/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/comport/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/shell/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/commands/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/filesystem/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/filesystem/fat32/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/net/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/e1000.o: src/net/drivers/e1000.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/apps/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/platform/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/interrupts/%.s | $(BUILD)
	gcc -m32 -c $< -o $@

$(BUILD)/%.o: src/interrupts/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/memory/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/driver_api.o: src/drivers/driver_api.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/util/%.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/string.o: src/util/string.c | $(BUILD)
	gcc $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/font_psf.o: assets/fonts/cp850-8x16.psf | $(BUILD)
	objcopy -I binary -O elf32-i386 -B i386 $< $@

# -------------------------
# Link kernel
# -------------------------
KERNEL_OBJS = \
$(CORE_OBJS) \
$(INTERRUPT_OBJS) \
$(MEMORY_OBJS) \
$(DRIVER_OBJS) \
$(NETWORK_OBJS) \
$(FS_OBJS) \
$(APP_OBJS)

kernel.elf: $(KERNEL_OBJS) boot/linker.ld
	gcc $(LDFLAGS) $(KERNEL_OBJS) -o $(BUILD)/kernel.elf

# -------------------------
# ISO
# -------------------------
os.iso: kernel.elf boot/grub.cfg
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(BUILD)/kernel.elf $(ISO_DIR)/boot/kernel.elf
	cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(BUILD)/os.iso $(ISO_DIR)

# -------------------------
# Disk
# -------------------------
$(BUILD)/disk.img:
	truncate -s 64M $@
	printf 'label: dos\nunit: sectors\n\nstart=2048, size=129024, type=c, bootable\n' | sfdisk $@
	mkfs.fat -F 32 -n PRISMOS --offset=2048 $@

$(BUILD)/disk-secondary.img:
	truncate -s 2G $@
	printf 'label: dos\nunit: sectors\n\nstart=2048, size=4192256, type=c, bootable\n' | sfdisk $@
	mkfs.fat -F 32 -n SECONDARY --offset=2048 $@

$(BUILD)/disk.vhdx: $(BUILD)/disk.img
	qemu-img convert -f raw -O vhdx $< $@

# -------------------------
# RUN (NO KVM)
# -------------------------
check-disk-unmounted:
	@if mountpoint -q "$(DISK_MOUNT_POINT)" || find "$(BUILD)" -type f -name '*.qemu-nbd-state' -print -quit 2>/dev/null | grep -q .; then \
		echo "Disk image is mounted on the host; run make unmount-disk first." >&2; \
		exit 1; \
	fi

run: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS)

run-serial: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -serial stdio

run-serial-log: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -serial file:$(BUILD)/serial.log

run-vhdx: QEMU_DISK_IMAGE=$(BUILD)/disk.vhdx
run-vhdx: QEMU_DISK_FORMAT=vhdx
run-vhdx: check-disk-unmounted os.iso $(BUILD)/disk.vhdx
	$(QEMU) $(QEMU_FLAGS)

run-secondary: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE) $(BUILD)/disk-secondary.img
	$(QEMU) $(QEMU_FLAGS) -drive file=$(BUILD)/disk-secondary.img,format=raw,if=ide,index=1,media=disk

run-net: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -netdev user,id=net0 -device e1000,netdev=net0

run-net-serial: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -netdev user,id=net0 -device e1000,netdev=net0 -serial stdio

# -------------------------
# HOST IMAGE ACCESS
# -------------------------
mount-disk: $(QEMU_DISK_IMAGE)
	sudo bash tools/mount-disk-image.sh mount "$(QEMU_DISK_IMAGE)" "$(QEMU_DISK_FORMAT)" "$(DISK_MOUNT_POINT)" 1

unmount-disk:
	sudo bash tools/mount-disk-image.sh unmount "$(DISK_MOUNT_POINT)"

mount-vhdx: $(BUILD)/disk.vhdx
	sudo bash tools/mount-disk-image.sh mount "$(BUILD)/disk.vhdx" vhdx "$(DISK_MOUNT_POINT)" 1

unmount-vhdx:
	sudo bash tools/mount-disk-image.sh unmount "$(DISK_MOUNT_POINT)"

# -------------------------
# RUN (KVM - FAST)
# -------------------------
run-kvm: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -enable-kvm -cpu host -m 512M

run-kvm-serial: check-disk-unmounted os.iso $(QEMU_DISK_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -enable-kvm -cpu host -m 512M -serial stdio

# -------------------------
# Tools
# -------------------------
prismcc: tools/prismcc.c src/apps/app_format.h | $(BUILD)
	gcc -O2 -Wall -Wextra -Isrc $< -o $(BUILD)/prismcc

clean:
	@if mountpoint -q "$(DISK_MOUNT_POINT)" || find "$(BUILD)" -type f -name '*.qemu-nbd-state' -print -quit 2>/dev/null | grep -q .; then \
		echo "Disk image is mounted on the host; unmount it before make clean." >&2; \
		exit 1; \
	fi
	rm -rf $(BUILD)