#!/usr/bin/env python3
"""
embed_vm.py - embed one or more Jack VM projects into ALOS.

Usage:
  python tools/embed_vm.py <dir>
  python tools/embed_vm.py <dir> <game_name>                 # backward compatible
  python tools/embed_vm.py <dir1> <dir2> ...                 # multi-game
  python tools/embed_vm.py <game1>=<dir1> <game2>=<dir2> ... # explicit names

Examples:
  python tools/embed_vm.py ../games/JackGuess
  python tools/embed_vm.py ../games/jackpokemonV Pokemon
  python tools/embed_vm.py Pokemon=../games/jackpokemonV Guess=../games/JackGuess

Generated files:
  kernel/jack/jack_data.c
  kernel/jack/jack_data.h
  kernel/jack/jack_mount.c

Notes:
  - OS .vm files (Sys.vm, Screen.vm, ...) are skipped because they are native in ALOS.
  - Game files are namespaced in vm_store as: <Game>/<File>.vm
    This avoids collisions when multiple games all contain Main.vm, Game.vm, etc.
  - If a folder contains a .gba ROM, it is embedded as: <Game>.gba
  - If a cover image exists (cover*.png/jpg/jpeg/bmp), it is embedded as: <Game>/cover.<ext>
  - If a GBA BIOS is found (gba_bios.bin), it is embedded as: gba_bios.bin
"""

import glob
import os
import sys
import zipfile

OS_VM = {
    "Array.vm",
    "Keyboard.vm",
    "Math.vm",
    "Memory.vm",
    "Output.vm",
    "Screen.vm",
    "String.vm",
    "Sys.vm",
}

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp")
BIOS_NAMES = ("gba_bios.bin", "GBA_BIOS.BIN", "GBA_BIOS.bin")


def fail(msg):
    print(f"Erreur: {msg}")
    sys.exit(1)


def sanitize_game_name(name):
    out = []
    for ch in name.strip():
        if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in "._-":
            out.append(ch)
        else:
            out.append("_")
    value = "".join(out).strip("._-")
    return value or "Game"


def c_ident(prefix, text):
    out = [prefix]
    for ch in text:
        if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9"):
            out.append(ch)
        else:
            out.append("_")
    ident = "".join(out)
    if ident[-1].isdigit():
        ident += "_"
    return ident


def c_string(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def safe_ascii_filename(name, fallback):
    out = []
    for ch in name:
        if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in "._-":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip("._-")
    return cleaned or fallback


def extract_gba_from_zip_if_needed(vm_dir, game):
    zip_files = sorted(glob.glob(os.path.join(vm_dir, "**", "*.zip"), recursive=True))
    for zpath in zip_files:
        try:
            with zipfile.ZipFile(zpath, "r") as zf:
                gba_entries = [n for n in zf.namelist() if n.lower().endswith(".gba")]
                if not gba_entries:
                    continue

                # Prefer the largest GBA entry when multiple are present.
                gba_entries.sort(key=lambda n: zf.getinfo(n).file_size, reverse=True)
                chosen = gba_entries[0]
                base = safe_ascii_filename(os.path.basename(chosen), f"{game}.gba")
                if not base.lower().endswith(".gba"):
                    base += ".gba"
                out_path = os.path.join(vm_dir, base)

                with zf.open(chosen, "r") as src, open(out_path, "wb") as dst:
                    dst.write(src.read())

                print(f"  [zip] ROM extraite: {os.path.basename(zpath)} -> {base}")
                return out_path
        except Exception as e:
            print(f"  [warn] zip ignore '{zpath}': {e}")
            continue
    return None


def find_gba_bios_file(specs, root_dir):
    candidates = []
    seen = set()

    def add(path):
        p = os.path.normpath(path)
        if p in seen:
            return
        seen.add(p)
        candidates.append(p)

    for _, vm_dir in specs:
        for name in BIOS_NAMES:
            add(os.path.join(vm_dir, name))
            add(os.path.join(vm_dir, "bios", name))

    for name in BIOS_NAMES:
        add(os.path.join(root_dir, name))
        add(os.path.join(root_dir, "bios", name))
        add(os.path.join(os.getcwd(), name))
        add(os.path.join(os.getcwd(), "bios", name))

    for p in candidates:
        if os.path.isfile(p):
            return p
    return None


def emit_byte_array(data):
    if not data:
        return "    0x00"
    rows = [
        "    " + ",".join(f"0x{b:02x}" for b in data[i : i + 16])
        for i in range(0, len(data), 16)
    ]
    return ",\n".join(rows) + ",0x00"


def parse_specs(argv):
    args = argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    # Backward compatibility: embed_vm.py <dir> <game_name>
    if (
        len(args) == 2
        and "=" not in args[0]
        and "=" not in args[1]
        and os.path.isdir(args[0])
        and not os.path.isdir(args[1])
    ):
        game = sanitize_game_name(args[1])
        return [(game, args[0])]

    specs = []
    for arg in args:
        if "=" in arg:
            game_raw, vm_dir = arg.split("=", 1)
            game = sanitize_game_name(game_raw)
        else:
            vm_dir = arg
            game = sanitize_game_name(os.path.basename(os.path.normpath(vm_dir)))

        if not os.path.isdir(vm_dir):
            fail(f"'{vm_dir}' n'est pas un dossier")
        specs.append((game, vm_dir))

    return specs


def collect_game(game, vm_dir):
    all_files = sorted(glob.glob(os.path.join(vm_dir, "*.vm")))
    files = [f for f in all_files if os.path.basename(f) not in OS_VM]
    skipped = [os.path.basename(f) for f in all_files if os.path.basename(f) in OS_VM]
    rom_files = sorted(glob.glob(os.path.join(vm_dir, "**", "*.gba"), recursive=True))

    if not rom_files:
        extracted = extract_gba_from_zip_if_needed(vm_dir, game)
        if extracted:
            rom_files = [extracted]

    img_files = []
    for ext in IMAGE_EXTS:
        img_files.extend(glob.glob(os.path.join(vm_dir, f"**/*{ext}"), recursive=True))
        img_files.extend(glob.glob(os.path.join(vm_dir, f"**/*{ext.upper()}"), recursive=True))
    img_files = sorted(set(img_files))

    cover_files = [f for f in img_files if "cover" in os.path.basename(f).lower() or "jaquette" in os.path.basename(f).lower()]
    if not cover_files and img_files:
        cover_files = [img_files[0]]

    assets = []
    if rom_files:
        assets.append(("rom", rom_files[0]))
    if cover_files:
        assets.append(("cover", cover_files[0]))

    if not files and not rom_files:
        fail(f"aucun contenu embarquable dans '{vm_dir}' (ni .vm de jeu, ni .gba)")

    total = sum(os.path.getsize(f) for f in files) + sum(os.path.getsize(f) for _, f in assets)
    print(f"Jeu: '{game}'  -  {len(files)} fichiers .vm, {len(assets)} asset(s)  ({total:,} bytes)")
    for f in files:
        print(f"  {os.path.basename(f):30s}  {os.path.getsize(f):7d} bytes")
    for kind, f in assets:
        print(f"  {os.path.basename(f):30s}  {os.path.getsize(f):7d} bytes  [{kind}]")
    if skipped:
        print(f"  (ignores - OS natif C: {', '.join(skipped)})")

    return {
        "game": game,
        "dir": vm_dir,
        "files": files,
        "assets": assets,
        "has_vm": bool(files),
        "has_rom": bool(rom_files),
        "total": total,
    }


def main():
    specs = parse_specs(sys.argv)

    # Detect duplicate game names after sanitization.
    seen_games = set()
    for game, _ in specs:
        if game in seen_games:
            fail(f"nom de jeu en doublon: '{game}'")
        seen_games.add(game)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.normpath(os.path.join(script_dir, ".."))
    bios_path = find_gba_bios_file(specs, root_dir)

    games = [collect_game(game, vm_dir) for game, vm_dir in specs]
    if bios_path:
        bios_size = os.path.getsize(bios_path)
        print(f"BIOS GBA detecte: {os.path.basename(bios_path)}  ({bios_size:,} bytes)")

    out_dir = os.path.normpath(os.path.join(script_dir, "..", "kernel", "jack"))
    os.makedirs(out_dir, exist_ok=True)

    lines = [
        "/* kernel/jack/jack_data.c - generated by tools/embed_vm.py */",
        '#include "jack_data.h"',
        '#include "../lib/string.h"',
        "",
    ]
    entries = []
    used_store_names = set()

    grand_total = 0
    total_vm_files = 0
    total_assets = 0

    for gidx, game in enumerate(games):
        gname = game["game"]
        lines.append(f"/* ===== Game: {gname} ===== */")

        vm_store_names = []
        for fidx, fpath in enumerate(game["files"]):
            base = os.path.basename(fpath)
            store_name = f"{gname}/{base}"
            if store_name in used_store_names:
                fail(f"collision de nom vm_store: '{store_name}'")
            used_store_names.add(store_name)
            vm_store_names.append(store_name)

            varname = c_ident("vm_", f"{gname}_{base}_{gidx}_{fidx}")
            with open(fpath, "rb") as f:
                data = f.read()
            lines += [
                f"/* {store_name} */",
                f"static const char {varname}[] = {{",
                emit_byte_array(data),
                "};",
                "",
            ]
            entries.append((store_name, varname, len(data)))
            total_vm_files += 1
            grand_total += len(data)

        for aidx, (akind, apath) in enumerate(game["assets"]):
            abase = os.path.basename(apath)
            ext = os.path.splitext(abase)[1].lower()
            if akind == "rom":
                store_name = f"{gname}.gba"
            else:
                if not ext:
                    ext = ".bin"
                store_name = f"{gname}/cover{ext}"

            if store_name in used_store_names:
                fail(f"collision de nom vm_store: '{store_name}'")
            used_store_names.add(store_name)

            varname = c_ident("vm_", f"{gname}_{abase}_asset_{gidx}_{aidx}")
            with open(apath, "rb") as f:
                data = f.read()
            lines += [
                f"/* {store_name} [{akind}] */",
                f"static const char {varname}[] = {{",
                emit_byte_array(data),
                "};",
                "",
            ]
            entries.append((store_name, varname, len(data)))
            total_assets += 1
            grand_total += len(data)

        if vm_store_names:
            vmdir_name = f"{gname}.vmdir"
            vmdir_content = "\n".join(vm_store_names) + "\n"
            vmdir_bytes = vmdir_content.encode("utf-8")
            vmdir_var = c_ident("vm_", f"{gname}_vmdir_{gidx}")
            lines += [
                f"/* {vmdir_name} - manifest for game {gname} */",
                f"static const char {vmdir_var}[] = {{",
                emit_byte_array(vmdir_bytes),
                "};",
                "",
            ]
            entries.append((vmdir_name, vmdir_var, len(vmdir_bytes)))

    if bios_path:
        store_name = "gba_bios.bin"
        if store_name in used_store_names:
            fail(f"collision de nom vm_store: '{store_name}'")
        used_store_names.add(store_name)

        with open(bios_path, "rb") as f:
            bios_data = f.read()
        bios_var = c_ident("vm_", "gba_bios_bin")
        lines += [
            f"/* {store_name} [bios] */",
            f"static const char {bios_var}[] = {{",
            emit_byte_array(bios_data),
            "};",
            "",
        ]
        entries.append((store_name, bios_var, len(bios_data)))
        total_assets += 1
        grand_total += len(bios_data)

    lines += ["const JackDataEntry jack_data_table[] = {"]
    for name, var, sz in entries:
        lines.append(f'    {{ "{c_string(name)}", {var}, {sz} }},')
    lines += [
        "    { 0, 0, 0 }",
        "};",
        f"const int jack_data_count = {len(entries)};",
    ]

    with open(os.path.join(out_dir, "jack_data.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    with open(os.path.join(out_dir, "jack_data.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(
            """#ifndef KERNEL_JACK_JACK_DATA_H
#define KERNEL_JACK_JACK_DATA_H
#include <stdint.h>
/* Embedded VM files table in .rodata */
typedef struct {
    const char *name;
    const char *data;
    uint32_t    size;
} JackDataEntry;
extern const JackDataEntry jack_data_table[];
extern const int           jack_data_count;
/* Called at boot: load embedded VM files into vm_store */
void jack_data_mount(void);
#endif
"""
        )

    with open(os.path.join(out_dir, "jack_mount.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(
            """/* kernel/jack/jack_mount.c - generated by tools/embed_vm.py */
#include "jack_data.h"
#include "vm_store.h"
void jack_data_mount(void) {
    vm_store_clear();
    for (int i = 0; jack_data_table[i].name; i++) {
        const JackDataEntry *e = &jack_data_table[i];
        vm_store_add(e->name, e->data, e->size);
    }
}
"""
        )

    print("")
    print(f"Embed termine: {len(games)} jeu(x), {total_vm_files} VM, {total_assets} asset(s), {grand_total:,} bytes")
    print("Fichiers generes:")
    print("  kernel/jack/jack_data.c")
    print("  kernel/jack/jack_data.h")
    print("  kernel/jack/jack_mount.c")
    print("")
    print("Relance:")
    print("  make run")
    print("")
    print("Dans QEMU:")
    print("  vmls")
    for g in games:
        if g["has_vm"]:
            print(f"  jack {g['game']}")
        if g["has_rom"]:
            print(f"  open {g['game']}.gba")
    print("")
    print("Note: VM files are namespaced as <Jeu>/<Fichier>.vm to avoid collisions.")


if __name__ == "__main__":
    main()

