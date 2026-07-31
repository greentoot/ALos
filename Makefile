CC = gcc
AS = nasm
LD = ld
QEMU = qemu-system-i386

CFLAGS = -m32 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector \
         -fno-pie -fno-pic -Wall -Wextra -O3 -fomit-frame-pointer -I. \
         -DALOS_HW_SAFE=1 -DALOS_FORCE_SMALL_HEAP=1 \
         -DALOS_STRICT_BAREMETAL=$(STRICT_BAREMETAL) \
         -DALOS_KERNEL_LINUX_LIKE=1
ASFLAGS = -f elf32 -DALOS_HW_SAFE=1
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib --no-warn-rwx-segments

STRICT_BAREMETAL ?= 0
TARGET = alos.elf
ISO_TARGET ?= alos-linux-like.iso
GRUB_CFG = grub/grub.cfg
ISO_SCRIPT = tools/build_iso.sh
RUN_QEMU_SCRIPT = tools/run_qemu.sh

QEMU_MEM ?= 512
QEMU_ACCEL ?= tcg
QEMU_DISPLAY ?= sdl
QEMU_BASE_FLAGS = -name ALOS-clean -accel $(QEMU_ACCEL) -vga std -m $(QEMU_MEM)

OBJS = \
	kernel_entry.o \
	kernel.o \
	driver/vga.o \
	driver/gfx.o \
	driver/serial.o \
	driver/usb_probe.o \
	driver/usb_xhci.o \
	driver/usb_hid_kbd.o \
	driver/timer.o \
	driver/keyboard.o \
	driver/mouse.o \
	driver/idt_stubs.o \
	kernel/tty.o \
	kernel/shell_linux_like.o \
	kernel/boot/bootinfo.o \
	kernel/boot/earlydiag.o \
	kernel/boot/fbmap.o \
	kernel/lib/string.o \
	kernel/lib/kprintf.o \
	kernel/lib/libc_compat.o \
	kernel/memory/pmm.o \
	kernel/memory/heap.o \
	kernel/process/task.o \
	kernel/process/scheduler.o \
	kernel/process/switch_ctx.o \
	kernel/fs/ramfs.o \
	kernel/fs/vfs.o \
	kernel/syscall/syscall.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

iso: $(TARGET) $(GRUB_CFG) $(ISO_SCRIPT)
	bash $(ISO_SCRIPT) $(ISO_TARGET) $(TARGET) $(GRUB_CFG)

iso-linux-like: clean iso

run-iso: iso
	bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY) -cdrom $(ISO_TARGET) -boot d

run: run-iso

debug: $(TARGET)
	$(QEMU) $(QEMU_BASE_FLAGS) -display none -kernel $(TARGET) -serial stdio 2>&1 | head -60

clean:
	rm -f $(OBJS) $(TARGET) alos.iso alos-linux-like.iso alos.img alos_persist.img
	rm -rf build

.PHONY: all iso iso-linux-like run run-iso debug clean
