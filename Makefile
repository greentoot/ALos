# ─── ALOS v0.5 Makefile ────────────────────────────────────────────────────
CC  = gcc
CXX = g++
AS  = nasm
LD  = ld
QEMU = qemu-system-i386

CFLAGS  = -m32 -std=c99 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pie -fno-pic -Wall -Wextra -O3 -fomit-frame-pointer -I.
CXXFLAGS = -m32 -std=gnu++17 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pie -fno-pic -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -fno-use-cxa-atexit -Wall -Wextra -O3 -fomit-frame-pointer -I.
MELONDS_INTERNAL_CXXFLAGS = $(filter-out -ffreestanding,$(CXXFLAGS)) \
           -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -I$(MELONDS_SRC_DIR)/src -I$(MELONDS_BUILD32_DIR)/src -DMELONDS_DS_RETAIL_ONLY=1
ASFLAGS = -f elf32
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib --no-warn-rwx-segments
STRICT_BAREMETAL ?= 0
HW_SAFE ?= 0
HW_SAFE_EMBED_ROM_LIBRARY ?= 0
NDS_INTERNAL_CORE ?= 1
EMBED_ROM_LIBRARY ?= 1
NDS_AUTOPROBE_ROM ?=

ifeq ($(HW_SAFE),1)
NDS_INTERNAL_CORE = 0
ifeq ($(HW_SAFE_EMBED_ROM_LIBRARY),1)
CFLAGS += -DALOS_HW_SAFE_GAMES=1
CXXFLAGS += -DALOS_HW_SAFE_GAMES=1
else
EMBED_ROM_LIBRARY = 0
endif
CFLAGS += -DALOS_HW_SAFE=1
CXXFLAGS += -DALOS_HW_SAFE=1
CFLAGS += -DALOS_FORCE_SMALL_HEAP=1
CXXFLAGS += -DALOS_FORCE_SMALL_HEAP=1
ASFLAGS += -DALOS_HW_SAFE=1
endif

MGBA_BUILD_DIR    = third_party/mgba/build-alos-kernel2
MGBA_LIB          = $(MGBA_BUILD_DIR)/libmgba.a
MGBA_BUILD_SCRIPT = tools/build_mgba_kernel.sh
MELONDS_SRC_DIR   = third_party/melonds
MELONDS_BUILD_DIR = $(MELONDS_SRC_DIR)/build-alos-core
MELONDS_LIB       = $(MELONDS_BUILD_DIR)/src/libcore.a
MELONDS_BUILD_SCRIPT = tools/build_melonds_core.sh
MELONDS_BUILD32_DIR = $(MELONDS_SRC_DIR)/build-alos-core32
MELONDS_LIB32       = $(MELONDS_BUILD32_DIR)/src/libcore.a
MELONDS_BUILD32_SCRIPT = tools/build_melonds_core32.sh
MELONDS_INTERNAL_PROBE_SCRIPT = tools/build_melonds_internal_probe.sh
MELONDS_RUNNER    = build/melonds_min/melonds_runner
MELONDS_RUNNER_BUILD_SCRIPT = tools/build_melonds_runner.sh
LIBSTDCXX32      := $(shell bash -lc 'g++ -m32 -print-file-name=libstdc++.a')
PYTHON_GEN        := $(shell if command -v python.exe >/dev/null 2>&1; then printf python.exe; else printf python3; fi)

CFLAGS += -DALOS_STRICT_BAREMETAL=$(STRICT_BAREMETAL)
CXXFLAGS += -DALOS_STRICT_BAREMETAL=$(STRICT_BAREMETAL)
ifneq ($(strip $(NDS_AUTOPROBE_ROM)),)
CFLAGS += -DALOS_NDS_AUTOPROBE_ROM=\"$(NDS_AUTOPROBE_ROM)\"
endif
CFLAGS += -DM_CORE_GBA -DM_CORE_GB -DMINIMAL_CORE=2
CXXFLAGS += -DM_CORE_GBA -DM_CORE_GB -DMINIMAL_CORE=2
CFLAGS += -Ithird_party/mgba/include -I$(MGBA_BUILD_DIR)/include
CXXFLAGS += -Ithird_party/mgba/include -I$(MGBA_BUILD_DIR)/include
GBA_AUTO_STAGE_DIR = build/gba_auto
GBA_AUTO_GEN_C     = build/gba_assets_generated.c
GBA_AUTO_GEN_MK    = build/gba_assets_generated.mk
GBA_AUTO_GEN_TOOL  = tools/gen_gba_assets.py
GBA_AUTO_ROM_EXTS ?=
GBA_AUTO_ROM_EXT_ARGS =
ifneq ($(strip $(GBA_AUTO_ROM_EXTS)),)
GBA_AUTO_ROM_EXT_ARGS = --rom-exts $(GBA_AUTO_ROM_EXTS)
endif

GBA_BIOS_SOURCE = $(firstword \
	$(wildcard bios/gba_bios.bin) \
	$(wildcard gba_bios.bin) \
	$(wildcard GBA_BIOS.BIN) \
	$(wildcard GBA_BIOS.bin))
GB_BIOS_SOURCE = $(firstword \
	$(wildcard bios/gb/gb_bios.bin) \
	$(wildcard gb_bios.bin) \
	$(wildcard GB_BIOS.BIN) \
	$(wildcard GB_BIOS.bin))
GB_DMG0_BIOS_SOURCE = $(firstword \
	$(wildcard bios/gb/dmg0_rom.bin) \
	$(wildcard dmg0_rom.bin) \
	$(wildcard DMG0_ROM.BIN) \
	$(wildcard DMG0_ROM.bin))
GBC_BIOS_SOURCE = $(firstword \
	$(wildcard bios/gbc/gbc_bios.bin) \
	$(wildcard gbc_bios.bin) \
	$(wildcard GBC_BIOS.BIN) \
	$(wildcard GBC_BIOS.bin))
NDS_BIOS7_SOURCE = $(firstword \
	$(wildcard bios/nds/biosnds7.rom) \
	$(wildcard biosnds7.rom) \
	$(wildcard BIOSNDS7.ROM))
NDS_BIOS9_SOURCE = $(firstword \
	$(wildcard bios/nds/biosnds9.rom) \
	$(wildcard biosnds9.rom) \
	$(wildcard BIOSNDS9.ROM))
NDS_FIRMWARE_SOURCE = $(firstword \
	$(wildcard bios/nds/firmware.bin) \
	$(wildcard firmware.bin) \
	$(wildcard FIRMWARE.BIN))
NDS_KEYCFG_SOURCE = $(firstword \
	$(wildcard bios/nds/key.cfg) \
	$(wildcard key.cfg) \
	$(wildcard KEY.CFG))
GBA_BIOS_OBJS =

ifneq ($(strip $(GBA_BIOS_SOURCE)),)
CFLAGS += -DALOS_HAS_GBA_BIOS=1
GBA_BIOS_OBJS += assets/gba_bios.o
endif

ifneq ($(strip $(GB_BIOS_SOURCE)),)
CFLAGS += -DALOS_HAS_GB_BIOS=1
GBA_BIOS_OBJS += assets/gb_bios.o
endif

ifneq ($(strip $(GB_DMG0_BIOS_SOURCE)),)
CFLAGS += -DALOS_HAS_GB_DMG0_BIOS=1
GBA_BIOS_OBJS += assets/dmg0_rom.o
endif

ifneq ($(strip $(GBC_BIOS_SOURCE)),)
CFLAGS += -DALOS_HAS_GBC_BIOS=1
GBA_BIOS_OBJS += assets/gbc_bios.o
endif

ifneq ($(strip $(NDS_BIOS7_SOURCE)),)
CFLAGS += -DALOS_HAS_NDS_BIOS7=1
GBA_BIOS_OBJS += assets/biosnds7.o
endif

ifneq ($(strip $(NDS_BIOS9_SOURCE)),)
CFLAGS += -DALOS_HAS_NDS_BIOS9=1
GBA_BIOS_OBJS += assets/biosnds9.o
endif

ifneq ($(strip $(NDS_FIRMWARE_SOURCE)),)
CFLAGS += -DALOS_HAS_NDS_FIRMWARE=1
GBA_BIOS_OBJS += assets/firmware_nds.o
endif

ifneq ($(strip $(NDS_KEYCFG_SOURCE)),)
CFLAGS += -DALOS_HAS_NDS_KEYCFG=1
GBA_BIOS_OBJS += assets/nds_keycfg.o
endif

ifneq ($(strip $(wildcard $(MELONDS_LIB) $(MELONDS_LIB32))),)
CFLAGS += -DALOS_HAS_MELONDS_CORE_BUILD=1
endif

ifeq ($(NDS_INTERNAL_CORE),1)
CFLAGS += -DALOS_HAS_NDS_INTERNAL_CORE=1
CXXFLAGS += -DALOS_HAS_NDS_INTERNAL_CORE=1
NDS_CORE_OBJS = \
	kernel/nds/melonds_platform_alos.o \
	kernel/nds/nds_core_melonds.o
NDS_CORE_LIBS = $(MELONDS_LIB32) $(LIBSTDCXX32)
else
NDS_CORE_OBJS = kernel/nds/nds_core_stub.o
NDS_CORE_LIBS =
endif

ifeq ($(EMBED_ROM_LIBRARY),1)
ifeq (,$(filter clean,$(MAKECMDGOALS)))
$(shell $(PYTHON_GEN) $(GBA_AUTO_GEN_TOOL) --project-root . --out-c $(GBA_AUTO_GEN_C) --out-mk $(GBA_AUTO_GEN_MK) --stage-dir $(GBA_AUTO_STAGE_DIR) $(GBA_AUTO_ROM_EXT_ARGS) >/dev/null)
-include $(GBA_AUTO_GEN_MK)
else
GBA_AUTO_OBJS :=
endif
JACK_ASSETS_GENERATED_OBJ = build/gba_assets_generated.o
else
GBA_AUTO_OBJS :=
JACK_ASSETS_GENERATED_OBJ = kernel/jack/jack_assets_generated_empty.o
endif

ifeq ($(HW_SAFE),1)
JACK_VM_RUNTIME_OBJS = kernel/jack/vm_interp_stub.o
JACK_DATA_RUNTIME_OBJS = kernel/jack/jack_data_empty.o
else
JACK_VM_RUNTIME_OBJS = kernel/jack/vm_interp.o
JACK_DATA_RUNTIME_OBJS = \
    kernel/jack/jack_data.o \
    kernel/jack/jack_mount.o
endif

OBJS = \
    kernel_entry.o \
    kernel.o \
    driver/vga.o \
    driver/gfx.o \
    driver/serial.o \
    driver/audio.o \
    driver/usb_probe.o \
    driver/usb_xhci.o \
    driver/usb_hid_kbd.o \
    driver/ata.o \
    driver/timer.o \
    driver/keyboard.o \
    driver/mouse.o \
    driver/idt_stubs.o \
    kernel/tty.o \
    kernel/shell.o \
    kernel/boot/bootinfo.o \
    kernel/boot/earlydiag.o \
    kernel/boot/fbmap.o \
    kernel/lib/string.o \
    kernel/lib/kprintf.o \
    kernel/lib/libc_compat.o \
    kernel/lib/cxx_runtime.o \
    kernel/memory/pmm.o \
    kernel/memory/heap.o \
    kernel/process/task.o \
    kernel/process/scheduler.o \
    kernel/process/switch_ctx.o \
    kernel/fs/ramfs.o \
    kernel/fs/vfs.o \
    kernel/fs/persist.o \
    kernel/install/installer.o \
    kernel/syscall/syscall.o \
    kernel/exec/asm_exec.o \
    kernel/exec/jackc.o \
    $(JACK_VM_RUNTIME_OBJS) \
    kernel/jack/vm_store.o \
    $(JACK_DATA_RUNTIME_OBJS) \
    kernel/jack/jack_assets.o \
	$(JACK_ASSETS_GENERATED_OBJ) \
	kernel/gba/gba_rominfo.o \
	kernel/gba/gba_mgba.o \
	$(NDS_CORE_OBJS) \
	kernel/nds/nds_backend.o \
	$(GBA_AUTO_OBJS)\
	$(GBA_BIOS_OBJS)

$(OBJS): $(MGBA_LIB)

kernel/nds/melonds_platform_alos.o kernel/nds/nds_core_melonds.o: $(MELONDS_LIB32)

kernel/nds/melonds_platform_alos.o: kernel/nds/melonds_platform_alos.cpp kernel/nds/melonds_platform_alos.h
	$(CXX) $(MELONDS_INTERNAL_CXXFLAGS) -c $< -o $@

kernel/nds/nds_core_melonds.o: kernel/nds/nds_core_melonds.cpp kernel/nds/nds_core_melonds.h kernel/nds/nds_core.h kernel/nds/melonds_platform_alos.h
	$(CXX) $(MELONDS_INTERNAL_CXXFLAGS) -c $< -o $@

TARGET = alos.elf
ISO_TARGET ?= alos.iso
IMG_TARGET = alos.img
PERSIST_IMG_TARGET = alos_persist.img
PERSIST_IMG_MB = 64
GRUB_CFG = grub/grub.cfg
ISO_SCRIPT = tools/build_iso.sh
DISK_SCRIPT = tools/build_disk_image.sh
PERSIST_SCRIPT = tools/build_persist_image.sh
RUN_QEMU_SCRIPT = tools/run_qemu.sh
HOST_BRIDGE_ENV = ALOS_ENABLE_HOST_BRIDGE=1
ifeq ($(NDS_INTERNAL_CORE),1)
QEMU_MEM ?= 1024
else
QEMU_MEM ?= 512
endif
# WSL/QEMU: thread=single peut provoquer qemu_mutex_lock_iothread_impl.
QEMU_ACCEL = tcg
QEMU_DISPLAY_PRIMARY = sdl
QEMU_NAME ?= ALOS
QEMU_BASE_FLAGS = -name $(QEMU_NAME) -accel $(QEMU_ACCEL) -vga std -m $(QEMU_MEM)
QEMU_DRIVE_FLAGS = -drive file=$(PERSIST_IMG_TARGET),format=raw,if=ide,index=0,media=disk

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

assets/gba_bios.bin: $(GBA_BIOS_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/gba_bios.o: assets/gba_bios.bin
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/gb_bios.bin: $(GB_BIOS_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/gb_bios.o: assets/gb_bios.bin
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/dmg0_rom.bin: $(GB_DMG0_BIOS_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/dmg0_rom.o: assets/dmg0_rom.bin
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/gbc_bios.bin: $(GBC_BIOS_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/gbc_bios.o: assets/gbc_bios.bin
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/biosnds7.rom: $(NDS_BIOS7_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/biosnds7.o: assets/biosnds7.rom
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/biosnds9.rom: $(NDS_BIOS9_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/biosnds9.o: assets/biosnds9.rom
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/firmware.bin: $(NDS_FIRMWARE_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/firmware_nds.o: assets/firmware.bin
	$(LD) -r -m elf_i386 -b binary -o $@ $<

assets/key.cfg: $(NDS_KEYCFG_SOURCE)
	mkdir -p assets
	cp "$<" "$@"

assets/nds_keycfg.o: assets/key.cfg
	$(LD) -r -m elf_i386 -b binary -o $@ $<

all: $(TARGET)

$(TARGET): $(OBJS) $(MGBA_LIB) $(NDS_CORE_LIBS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(MGBA_LIB) $(NDS_CORE_LIBS)

$(MGBA_LIB): $(MGBA_BUILD_SCRIPT) third_party/mgba/CMakeLists.txt
	bash $(MGBA_BUILD_SCRIPT)

run: $(TARGET) $(PERSIST_IMG_TARGET)
	bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -kernel $(TARGET) $(QEMU_DRIVE_FLAGS)

run-dev-host: $(TARGET) $(PERSIST_IMG_TARGET)
	$(HOST_BRIDGE_ENV) bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -kernel $(TARGET) $(QEMU_DRIVE_FLAGS)

run-stub:
	$(MAKE) run NDS_INTERNAL_CORE=0

NDS_INTERNAL_REFRESH_OBJS = \
	kernel/shell.o \
	kernel/nds/nds_backend.o \
	kernel/nds/melonds_platform_alos.o \
	kernel/nds/nds_core_melonds.o \
	kernel/lib/cxx_runtime.o \
	$(TARGET)

nds-internal-refresh:
	rm -f $(NDS_INTERNAL_REFRESH_OBJS)

all-nds-internal:
	$(MAKE) nds-internal-refresh
	$(MAKE) all NDS_INTERNAL_CORE=1

run-nds-internal:
	$(MAKE) nds-internal-refresh
	$(MAKE) run NDS_INTERNAL_CORE=1

run-nds-autoprobe:
	@if [ -z "$(ROM)" ]; then \
		echo "Utilise: make run-nds-autoprobe ROM=PokemonPlatine.nds"; \
		exit 1; \
	fi
	@echo "[ALOS] Rebuild force du backend DS interne"
	$(MAKE) nds-internal-refresh
	$(MAKE) run NDS_INTERNAL_CORE=1 NDS_AUTOPROBE_ROM='$(ROM)'

run-nds-debug:
	$(MAKE) run-nds-autoprobe ROM='$(ROM)'

run-iso-nds-internal:
	$(MAKE) nds-internal-refresh
	$(MAKE) run-iso NDS_INTERNAL_CORE=1

run-disk-nds-internal:
	$(MAKE) nds-internal-refresh
	$(MAKE) run-disk NDS_INTERNAL_CORE=1

debug-nds-internal:
	$(MAKE) nds-internal-refresh
	$(MAKE) debug NDS_INTERNAL_CORE=1

run-persist: $(TARGET) $(PERSIST_IMG_TARGET)
	$(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -kernel $(TARGET) $(QEMU_DRIVE_FLAGS)

melonds-core:
	bash $(MELONDS_BUILD_SCRIPT) $(MELONDS_SRC_DIR) $(MELONDS_BUILD_DIR)

melonds-core32:
	bash $(MELONDS_BUILD32_SCRIPT) $(MELONDS_SRC_DIR) $(MELONDS_BUILD32_DIR)

melonds-status:
	@bash -lc 'if [ -f "$(MELONDS_LIB)" ]; then \
		echo "melonDS core: present ($(MELONDS_LIB))"; \
	else \
		echo "melonDS core: absent (utilise: make melonds-core)"; \
	fi'

melonds-status32:
	@bash -lc 'if [ -f "$(MELONDS_LIB32)" ]; then \
		echo "melonDS core 32-bit: present ($(MELONDS_LIB32))"; \
	else \
		echo "melonDS core 32-bit: absent (utilise: make melonds-core32)"; \
	fi'

melonds-clean:
	rm -rf $(MELONDS_BUILD_DIR)

melonds-clean32:
	rm -rf $(MELONDS_BUILD32_DIR)

melonds-runner: $(MELONDS_LIB) $(MELONDS_RUNNER_BUILD_SCRIPT) host/melonds_min/platform_minimal.cpp host/melonds_min/platform_minimal.h host/melonds_min/runner.cpp
	bash $(MELONDS_RUNNER_BUILD_SCRIPT) $(MELONDS_SRC_DIR) $(MELONDS_BUILD_DIR) $(MELONDS_RUNNER)

melonds-run: melonds-runner
	@if [ -z "$(ROM)" ]; then \
		echo "Utilise: make melonds-run ROM=rom_library/ton_jeu/Jeu.nds"; \
		exit 1; \
	fi
	bash -lc '"$(MELONDS_RUNNER)" "$(ROM)" --project-root "$$(pwd)"'

melonds-internal-probe: $(MELONDS_LIB32) $(MELONDS_INTERNAL_PROBE_SCRIPT) kernel/lib/cxx_runtime.cpp kernel/nds/melonds_platform_alos.cpp kernel/nds/melonds_platform_alos.h kernel/nds/nds_core_melonds.cpp kernel/nds/nds_core_melonds.h
	bash $(MELONDS_INTERNAL_PROBE_SCRIPT) $(MELONDS_SRC_DIR) $(MELONDS_BUILD32_DIR) build/melonds_internal_probe

nds-kernel-linktest:
	$(MAKE) all NDS_INTERNAL_CORE=1

rom-scan:
	$(PYTHON_GEN) $(GBA_AUTO_GEN_TOOL) --project-root . --out-c $(GBA_AUTO_GEN_C) --out-mk $(GBA_AUTO_GEN_MK) --stage-dir $(GBA_AUTO_STAGE_DIR) $(GBA_AUTO_ROM_EXT_ARGS)

debug: $(TARGET) $(PERSIST_IMG_TARGET)
	$(QEMU) $(QEMU_BASE_FLAGS) -display none -kernel $(TARGET) $(QEMU_DRIVE_FLAGS) -serial stdio 2>&1 | head -40

iso: $(TARGET) $(GRUB_CFG) $(ISO_SCRIPT)
	bash $(ISO_SCRIPT) $(ISO_TARGET) $(TARGET) $(GRUB_CFG)

iso-hw-safe:
	$(MAKE) clean
	$(MAKE) iso HW_SAFE=1
	cp $(ISO_TARGET) alos-hw-safe.iso

iso-hw-games:
	$(MAKE) clean
	$(MAKE) iso HW_SAFE=1 HW_SAFE_EMBED_ROM_LIBRARY=1 GBA_AUTO_ROM_EXTS=.gb,.gbc,.gba
	cp $(ISO_TARGET) alos-hw-games.iso

iso-installer: disk $(TARGET) $(GRUB_CFG) $(ISO_SCRIPT)
	bash $(ISO_SCRIPT) $(ISO_TARGET) $(TARGET) $(GRUB_CFG) $(IMG_TARGET)

disk: $(TARGET) $(GRUB_CFG) $(DISK_SCRIPT)
	bash $(DISK_SCRIPT) $(IMG_TARGET) $(TARGET) $(GRUB_CFG)

run-iso: iso $(PERSIST_IMG_TARGET)
	bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -cdrom $(ISO_TARGET) -hda $(PERSIST_IMG_TARGET) -boot d

run-iso-installer: iso-installer $(PERSIST_IMG_TARGET)
	bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -cdrom $(ISO_TARGET) -hda $(PERSIST_IMG_TARGET) -boot d

run-iso-dev-host: iso $(PERSIST_IMG_TARGET)
	$(HOST_BRIDGE_ENV) bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -cdrom $(ISO_TARGET) -hda $(PERSIST_IMG_TARGET) -boot d

run-disk: disk
	bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -hda $(IMG_TARGET) -boot c

run-disk-dev-host: disk
	$(HOST_BRIDGE_ENV) bash $(RUN_QEMU_SCRIPT) $(QEMU) $(QEMU_BASE_FLAGS) -display $(QEMU_DISPLAY_PRIMARY) -hda $(IMG_TARGET) -boot c

$(PERSIST_IMG_TARGET): $(PERSIST_SCRIPT)
	bash $(PERSIST_SCRIPT) $(PERSIST_IMG_TARGET) $(PERSIST_IMG_MB)

persist-reset:
	rm -f $(PERSIST_IMG_TARGET)
	bash $(PERSIST_SCRIPT) $(PERSIST_IMG_TARGET) $(PERSIST_IMG_MB)

strict:
	$(MAKE) clean
	$(MAKE) all STRICT_BAREMETAL=1

clean:
	rm -f $(OBJS) $(TARGET) $(ISO_TARGET) $(IMG_TARGET) $(PERSIST_IMG_TARGET)
	rm -f assets/*.o assets/*.bin
	rm -rf build

.PHONY: all run run-stub run-dev-host nds-internal-refresh all-nds-internal run-nds-internal run-nds-autoprobe run-nds-debug run-iso-nds-internal run-disk-nds-internal debug-nds-internal run-persist melonds-core melonds-core32 melonds-status melonds-status32 melonds-clean melonds-clean32 melonds-runner melonds-run melonds-internal-probe nds-kernel-linktest rom-scan debug iso iso-installer disk run-iso run-iso-installer run-iso-dev-host run-disk run-disk-dev-host persist-reset strict clean embed

embed:
	$(PYTHON_GEN) tools/embed_vm.py $(VM_DIR)
	$(MAKE) all
