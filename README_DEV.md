# ALOS v0.5 - Developer Guide

This document is for contributors and maintainers of ALOS.
It focuses on how the system is built, how subsystems interact, and how to debug and extend it safely.

---

## 1) Scope and Current State

ALOS is a freestanding 32-bit x86 kernel with:

- Multiboot boot path (GRUB)
- text shell + graphical home launcher
- Hack VM runtime (Nand2Tetris) with embedded `.vm` projects
- native mGBA core integration for `.gba` ROM execution
- ATA-backed persistence snapshot for mutable RamFS + GBA saves

Primary runtime target today is QEMU i386.

---

## 2) Fast Start for Devs

### 2.1 Required tools (WSL/Linux)

Base build/runtime:

- `build-essential`
- `gcc-multilib`
- `g++-multilib`
- `nasm`
- `qemu-system-x86`
- `python3`
- `cmake`

ISO creation:

- `grub-pc-bin`
- `xorriso`

Disk image creation (`make disk`):

- `parted`
- `sfdisk` (util-linux)
- `losetup` (util-linux)
- `mkfs.ext2` (e2fsprogs)
- `mount` / `umount`
- `grub-install`

Recommended one-shot install on Debian/Ubuntu:

```bash
sudo apt-get update -y
sudo apt-get install -y \
  build-essential gcc-multilib g++-multilib nasm qemu-system-x86 \
  python3 cmake grub-pc-bin xorriso parted util-linux e2fsprogs
```

### 2.2 Core commands

```bash
make all           # build kernel ELF
make run           # run kernel + persistent disk attached
make debug         # boot trace in serial/stdout (head -40)
make iso           # build bootable ISO (GRUB)
make run-iso       # boot ISO + persistent disk attached
make disk          # build bootable disk image with GRUB + persist partition
make run-disk      # boot disk image
make persist-reset # wipe and recreate persistence image
```

Important:

- `make run` and `make run-iso` automatically attach `alos_persist.img`.
- The persistence image is preserved between runs unless `persist-reset` is executed.

---

## 3) Repository Map (Developer View)

Top-level critical files:

- `kernel.c`: kernel boot orchestration and init order
- `Makefile`: toolchain flags, targets, runtime launch commands
- `linker.ld`: ELF section layout and kernel memory anchor
- `kernel_entry.asm`: early entry point and jump to `kernel_main`

Subsystem roots:

- `driver/`: VGA, graphics, keyboard, timer, ATA
- `kernel/boot/`: multiboot parse + platform probes
- `kernel/memory/`: PMM + heap
- `kernel/fs/`: RamFS, VFS, persistence snapshot layer
- `kernel/exec/`: ASM executor and Jack compiler
- `kernel/jack/`: VM runtime, VM store, embedded game data
- `kernel/gba/`: mGBA runtime bridge and save integration
- `kernel/shell.c`: commands, launcher, open logic
- `tools/`: embed/build scripts

---

## 4) Boot and Runtime Flow

Boot sequence:

1. `kernel_entry.asm` sets early state and calls `kernel_main`
2. `bootinfo_parse` reads multiboot info + framebuffer + hypervisor hint
3. graphics mode preference set (if framebuffer metadata available)
4. text mode captured, TTY initialized
5. PMM + heap initialized
6. `ramfs_init` builds base tree
7. `persist_init` attempts restore from ATA partition type `0xA0`
8. VM and assets mounted (`jack_data_mount`, `jack_assets_mount`)
9. VFS, tasking, scheduler, PIT timer, keyboard initialized
10. shell loop starts

Primary command paths:

- `home` / `F1` -> graphical launcher
- `open <target>` -> transparent launch path (`.vm`, `.vmdir`, `.gba`, `.exe` placeholder)
- `jack ...` -> Hack VM runtime
- `.gba` launch -> mGBA core wrapper

---

## 5) Persistence Design (Critical)

Persistence is handled by `kernel/fs/persist.c` over raw ATA sectors.

### 5.1 Disk contract

- MBR partition with type `0xA0` is required.
- Snapshot is stored raw inside that partition.
- For default dev flow, `alos_persist.img` already contains such partition.

### 5.2 Snapshot format

Header:

- `magic`: `ALOSPST1`
- `version`: currently `1`
- `total_size`
- `crc32` of payload

Entry types:

- `ENTRY_RAMFS_DIR`
- `ENTRY_RAMFS_FILE`
- `ENTRY_GBA_SAVE`

### 5.3 Restore strategy

- mutable RamFS nodes are cleared
- GBA save slots are cleared
- directories are recreated first (multi-pass to satisfy parent-before-child)
- files and GBA saves restored in final pass
- dirty-mark callbacks are ignored while loading (`g_loading` guard)

### 5.4 Write triggers

`persist_mark_dirty()` is called by:

- RamFS create/update/remove/mkdir paths
- shell direct in-place updates (`write`, editors, `jackc` destination overwrite)
- GBA save set/store path

Flush behavior:

- immediate flush on dirty mark (simple and robust)
- `persist flush` shell command available

Diagnostic shell command:

```text
persist status
persist flush
```

---

## 6) Graphics and UI Architecture

There are three major visual modes:

1. VGA text shell (`driver/vga.*`, `kernel/tty.*`)
2. Hack-style VM graphics (512x256 indexed path in `driver/gfx.*`)
3. Home launcher UI (custom indexed renderer in `kernel/shell.c`)

Mode transitions must restore text state cleanly:

- `gfx_restore_text_mode()`
- `vga_init()`
- `tty_init()`

If a transition returns to black/garbled output, check that all three calls are executed in that order on every exit path.

---

## 7) VM and Game Content Pipeline

### 7.1 Embedding VM projects

`tools/embed_vm.py` generates:

- `kernel/jack/jack_data.c`
- `kernel/jack/jack_data.h`
- `kernel/jack/jack_mount.c`

Usage examples:

```bash
python3 tools/embed_vm.py ./jackpokemonV
python3 tools/embed_vm.py Pokemon=./jackpokemonV Guess=./jackguess
make all
```

Namespacing rule:

- VM files are stored as `<Game>/<File>.vm`
- a `<Game>.vmdir` manifest is generated for game launch convenience

### 7.2 Embedding GBA ROM + cover

If a game folder contains:

- a `.gba` file -> embedded as `<Game>.gba`
- a cover image (`cover*.png/jpg/jpeg/bmp`) -> embedded as `<Game>/cover.<ext>`

Runtime:

- `.gba` files appear in launcher / `open` path
- known cover asset is used by home UI tile rendering

---

## 8) mGBA Integration Notes

Build script:

- `tools/build_mgba_kernel.sh`

Static library output:

- `third_party/mgba/build-alos-kernel2/libmgba.a`

Runtime bridge:

- `kernel/gba/gba_mgba.c`

Current behavior:

- in-memory ROM load
- key mapping from ALOS keyboard events
- frame blit into indexed 512x256 canvas
- save extraction/restoration to persistence bridge

---

## 9) Nintendo DS Ramp-Up

ALOS now has a DS preflight layer in-kernel:

- `.nds` ROM header parsing
- DS BIOS/firmware/key.cfg asset probing
- graphical DS preflight modal from the home launcher
- `dsdiag` shell command for DS diagnostics

The actual DS emulation core is not linked into the kernel yet.

### 9.1 Optional official melonDS core checkout

An optional helper target prepares the official melonDS core checkout/build:

```bash
make melonds-core
make melonds-status
```

What this does:

- clones `https://github.com/melonDS-emu/melonDS` into `third_party/melonds` if absent
- builds a minimal static `libcore.a` in `third_party/melonds/build-alos-core/src/libcore.a`
- disables the Qt/SDL frontend and extra runtime pieces for a smaller build surface

Current use:

- validates that the upstream core can be built in this dev environment
- gives ALOS a concrete target for future native DS runtime integration

Current limitation:

- the library is not yet linked or adapted to the freestanding kernel runtime
- melonDS still expects a substantial `Platform::*` host layer (files, threads, semaphores, timing, save I/O, logging, etc.)

### 9.2 Why this intermediate step matters

This avoids two common traps:

- pretending DS is "almost done" when no real upstream core is actually buildable
- hard-forking a random emulator snapshot without a reproducible build path

With `make melonds-core`, contributors can now work against:

- a pinned local checkout path
- a reproducible upstream build command
- a clear integration gap between `libcore.a` and the kernel wrapper

### 9.3 Minimal host shim and runner

There is now a hosted validation path that links against upstream `libcore.a`
without waiting for full kernel integration:

```bash
make melonds-runner
```

This builds:

- `build/melonds_min/melonds_runner`

The runner provides:

- a minimal `melonDS::Platform::*` shim
- DS BIOS + firmware loading from `bios/nds/`
- `.sav` persistence next to the ROM
- a writable runtime firmware copy in `build/melonds_min/`
- a simple PPM framebuffer dump for frame validation

Typical usage when a ROM is available:

```bash
make melonds-run ROM=rom_library/pokemon_diamant/PokemonDiamant.nds
```

Or directly:

```bash
build/melonds_min/melonds_runner rom_library/pokemon_diamant/PokemonDiamant.nds --project-root "$(pwd)"
```

Expected output:

- top/bottom framebuffer hashes in stdout
- last frame dump in `build/melonds_min/last_frame.ppm`
- save file written next to the ROM as `<game>.sav`

This is intentionally a host-side step:

- it proves the core boots through our shim
- it narrows future ALOS kernel work to video/input/storage adaptation
- it keeps DS bring-up debuggable before attempting freestanding integration

---

## 10) Debugging Playbook

### 9.1 "Persist inactive"

Symptoms:

- `persist status` reports inactive

Checks:

1. run with `make run` or `make run-iso`
2. ensure disk has partition type `0xA0`
3. check ATA driver presence path (`ata_is_present`)

### 9.2 "State lost after reboot"

Checks:

1. confirm using persistent launch target (`make run`, not raw ad-hoc QEMU without drive)
2. call `persist flush` before reboot for explicit sync test
3. avoid `make persist-reset` unless intentional

### 9.3 "Black screen on ESC from game"

Check all exits call:

- `gfx_restore_text_mode()`
- `vga_init()`
- `tty_init()`

Also verify keyboard focus in QEMU window (`Ctrl+Alt+G` to release grab).

### 9.4 "Input lag in games"

Potential causes:

- game-side waits (`Sys.wait`) in VM scripts
- interpreter polling cadence / frame flush throttling
- host performance under WSL + QEMU

---

## 10) Security and Reliability Limits

Current known hard limits:

- no user/kernel isolation (everything ring0)
- no paging/MMU process isolation
- no privilege separation for runtime code paths
- no robust journaling filesystem (snapshot model only)
- no crash-consistent two-phase commit on snapshot writes

Practical implication:

- this is a dev OS playground, not a hardened production OS

---

## 11) Contribution Rules

### 11.1 Change discipline

- keep changes small and subsystem-local
- avoid mixing refactor + feature + behavior change in one patch
- preserve bootability and `make run` path

### 11.2 Before opening PR / sharing patch

Run at least:

```bash
make all
make run
```

And validate:

- shell is usable
- `home` opens
- one VM game launches
- one GBA ROM launches (if embedded)
- `persist status` active
- a file edit survives reboot

### 11.3 Style constraints

- C99 freestanding
- avoid libc dependencies unless routed via existing compat layer
- keep code ASCII by default
- comment only where behavior is non-obvious

---

## 12) Suggested Next Engineering Steps

Priority backlog:

1. Persistence hardening:
   - dual-slot snapshot (A/B) + generation counter
   - power-fail safer write strategy
2. Filesystem:
   - real on-disk FS abstraction beyond snapshot model
3. Process model:
   - user mode + syscall boundary hardening
4. Graphics:
   - unified mode manager API for all transitions
5. Tooling:
   - scripted smoke tests for boot, launch, persist

---

## 13) Reference Commands (Ops Cheat Sheet)

```bash
# full rebuild
make clean && make all

# run with persistence
make run

# inspect persistence image partition type
sfdisk -d alos_persist.img

# reset persistence state
make persist-reset

# embed multiple VM games
python3 tools/embed_vm.py Pokemon=./jackpokemonV Guess=./jackguess
make all
```

---

## 14) Ownership Pointers

When debugging, start here:

- boot/memory init issues: `kernel.c`, `kernel/boot/*`, `kernel/memory/*`
- keyboard/timing issues: `driver/keyboard.*`, `driver/timer.*`
- shell/launcher issues: `kernel/shell.c`, `kernel/tty.*`, `driver/gfx.*`
- VM runtime issues: `kernel/jack/vm_interp.*`, `kernel/jack/vm_store.*`
- GBA runtime issues: `kernel/gba/gba_mgba.*`
- persistence issues: `kernel/fs/persist.*`, `driver/ata.*`, `tools/build_persist_image.sh`
