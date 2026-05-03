#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path

try:
    from PIL import Image
except Exception:
    Image = None


LIBRARY_ROOTS = ("rom_library", "gba_library", "games/roms", "games/gba")
LEGACY_ROOTS = ("pokemon_rouge", "pokemon_emeraude")
AUTO_ROOT_PREFIXES = ("pokemon ", "pokémon ")
IGNORED_TOPLEVEL = {
    ".git",
    ".vscode",
    "__pycache__",
    "assets",
    "bios",
    "bootloader",
    "build",
    "driver",
    "grub",
    "jackguess",
    "jackpokemonV",
    "kernel",
    "third_party",
    "tools",
}
STOPWORDS = {"version", "the", "usa", "eur", "rev", "beta", "proto", "prototype"}
ROM_EXTENSIONS = (".gba", ".gbc", ".gb", ".nds", ".3ds", ".cia")
COVER_PREVIEW_W = 86
COVER_PREVIEW_H = 86
COVER_PREVIEW_BG_RGB = (244, 246, 248)
COVER_PREVIEW_PALETTE_RGB = [
    (0, 0, 0),
    (255, 255, 255),
    (226, 231, 236),
    (192, 198, 206),
    (145, 151, 160),
    (96, 102, 112),
    (204, 48, 52),
    (228, 112, 36),
    (236, 196, 44),
    (122, 172, 58),
    (42, 134, 70),
    (52, 160, 192),
    (56, 92, 188),
    (126, 92, 188),
    (164, 104, 60),
    (224, 160, 120),
]
GENERIC_CONTEXT_NAMES = {
    "rom",
    "roms",
    "games",
    "game",
    "gb",
    "gbc",
    "gba",
    "nds",
    "3ds",
    "cia",
    "zip",
    "assets",
    "covers",
    "images",
}


@dataclass
class GameAsset:
    title: str
    rom_stage: Path
    rom_ext: str
    cover_stage: Path | None
    cover_preview: bytes | None


@dataclass(frozen=True)
class PackageSource:
    source_path: Path
    context_dir: Path
    key_hint: str


def safe_rmtree(path: Path) -> None:
    try:
        shutil.rmtree(path, ignore_errors=True)
    except FileNotFoundError:
        pass


def strip_accents(text: str) -> str:
    return "".join(
        ch
        for ch in unicodedata.normalize("NFKD", text)
        if not unicodedata.combining(ch)
    )


def normalize_title(raw: str) -> str:
    s = strip_accents(raw)
    s = re.sub(r"pok[\W_]*mon", "Pokemon", s, flags=re.IGNORECASE)
    s = s.replace("&", " And ")
    tokens = re.findall(r"[A-Za-z0-9]+", s)
    filtered = [tok for tok in tokens if tok.lower() not in STOPWORDS]
    if filtered and filtered[-1].isdigit() and len(filtered[-1]) <= 4:
        filtered.pop()
    if not filtered:
        filtered = ["Game"]
    title = "".join(tok[:1].upper() + tok[1:] for tok in filtered)
    if title and title[0].isdigit():
        title = f"Gba{title}"
    return title[:80]


def slugify(text: str) -> str:
    s = strip_accents(text).lower()
    s = re.sub(r"[^a-z0-9]+", "_", s).strip("_")
    return s or "game"


def parse_rom_exts(value: str) -> tuple[str, ...]:
    if not value:
        return ROM_EXTENSIONS

    selected: list[str] = []
    allowed = set(ROM_EXTENSIONS)
    for raw in value.split(","):
        ext = raw.strip().lower()
        if not ext:
            continue
        if not ext.startswith("."):
            ext = f".{ext}"
        if ext in allowed and ext not in selected:
            selected.append(ext)
    return tuple(selected) if selected else ROM_EXTENSIONS


def discover_auto_roots(project_root: Path) -> list[Path]:
    roots: list[Path] = []

    for name in LIBRARY_ROOTS + LEGACY_ROOTS:
        path = project_root / name
        if path.exists():
            roots.append(path)

    for child in sorted(project_root.iterdir(), key=lambda p: p.name.lower()):
        if not child.is_dir():
            continue
        if child.name in IGNORED_TOPLEVEL:
            continue
        low = child.name.lower()
        if low.startswith(AUTO_ROOT_PREFIXES):
            roots.append(child)

    uniq: list[Path] = []
    seen = set()
    for path in roots:
        key = str(path.resolve())
        if key not in seen:
            seen.add(key)
            uniq.append(path)
    return uniq


def list_library_packages(root: Path) -> list[Path]:
    packages: list[Path] = []
    for child in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        if child.is_dir():
            packages.append(child)
        elif child.is_file() and child.suffix.lower() in set(ROM_EXTENSIONS) | {".zip"}:
            packages.append(child)
    return packages


def has_cover_assets(path: Path) -> bool:
    return (path / "cover.png").is_file() or (path / "images" / "cover.png").is_file()


def is_generic_context_name(name: str) -> bool:
    lowered = name.strip().lower()
    return not lowered or lowered in GENERIC_CONTEXT_NAMES or lowered.isdigit()


def ancestors_within_root(root: Path, start: Path) -> list[Path]:
    result: list[Path] = []
    current = start
    while True:
        if current == root or root in current.parents:
            result.append(current)
        else:
            break
        if current == root:
            break
        current = current.parent
    return result


def guess_context_dir(root: Path, source_path: Path) -> Path:
    if source_path.is_dir():
        return source_path

    ancestors = ancestors_within_root(root, source_path.parent)

    for ancestor in ancestors:
        if ancestor != root and has_cover_assets(ancestor):
            return ancestor

    for ancestor in ancestors:
        if ancestor == root:
            continue
        if not is_generic_context_name(ancestor.name):
            return ancestor

    return source_path.parent


def make_key_hint(root: Path, source_path: Path) -> str:
    try:
        rel = source_path.relative_to(root)
        parts = list(rel.parts[:-1]) + [source_path.stem]
    except ValueError:
        parts = [source_path.stem]
    cleaned = [slugify(part) for part in parts if part]
    return "_".join(part for part in cleaned if part) or slugify(source_path.stem)


def iter_library_sources(root: Path) -> list[PackageSource]:
    supported_suffixes = set(ROM_EXTENSIONS) | {".zip"}
    packages: list[PackageSource] = []

    for candidate in sorted(root.rglob("*"), key=lambda p: p.as_posix().lower()):
        if not candidate.is_file():
            continue
        if candidate.suffix.lower() not in supported_suffixes:
            continue
        if candidate.name.lower() == "cover.png":
            continue

        packages.append(
            PackageSource(
                source_path=candidate,
                context_dir=guess_context_dir(root, candidate),
                key_hint=make_key_hint(root, candidate),
            )
        )

    return packages


def find_rom_file(package: Path) -> Path | None:
    if package.is_file() and package.suffix.lower() in ROM_EXTENSIONS:
        return package
    if package.is_file():
        return None
    for ext in ROM_EXTENSIONS:
        candidates = sorted(package.rglob(f"*{ext}"))
        if candidates:
            return candidates[0]
    return None


def extract_rom_from_zip(zip_path: Path, out_file_stem: Path) -> tuple[str, Path, str] | None:
    try:
        with zipfile.ZipFile(zip_path) as zf:
            for ext in ROM_EXTENSIONS:
                infos = sorted(
                    (info for info in zf.infolist() if info.filename.lower().endswith(ext)),
                    key=lambda info: info.filename.lower(),
                )
                if not infos:
                    continue
                info = infos[0]
                out_file = out_file_stem.with_suffix(ext)
                out_file.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, out_file.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                return info.filename, out_file, ext
            return None
    except zipfile.BadZipFile:
        return None


def find_cover_file(context_dir: Path, source_path: Path, rom_path: Path | None) -> Path | None:
    if source_path.is_file():
        stem_png = source_path.with_suffix(".png")
        if stem_png.exists():
            return stem_png

    preferred = [
        context_dir / "cover.png",
        context_dir / "images" / "cover.png",
    ]
    if rom_path is not None:
        preferred.extend(
            [
                rom_path.with_suffix(".png"),
                rom_path.parent / "cover.png",
                context_dir / f"{rom_path.stem}.png",
            ]
        )
    if source_path.is_file():
        preferred.extend(
            [
                source_path.parent / "cover.png",
                source_path.parent / "images" / "cover.png",
            ]
        )
    for path in preferred:
        if path.exists() and path.is_file():
            return path

    extra = sorted(context_dir.rglob("cover.png"))
    if extra:
        return extra[0]
    return None


def choose_base_name(context_dir: Path, source_path: Path, rom_name: str | None) -> str:
    if rom_name:
        stem = Path(rom_name).stem
        if stem and not stem.isdigit():
            return stem
    if source_path.is_file() and source_path.stem and not source_path.stem.isdigit():
        return source_path.stem
    if context_dir.name and not is_generic_context_name(context_dir.name):
        return context_dir.name
    return source_path.stem if source_path.is_file() else source_path.name


def hash_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def build_cover_preview(cover_path: Path | None) -> bytes | None:
    if cover_path is None or Image is None:
        return None

    try:
        resampling = getattr(Image, "Resampling", Image)
        dither_mod = getattr(Image, "Dither", Image)
        resample_lanczos = getattr(resampling, "LANCZOS", Image.LANCZOS)
        dither_none = getattr(dither_mod, "NONE", 0)

        palette_image = Image.new("P", (1, 1))
        flat_palette: list[int] = []
        for r, g, b in COVER_PREVIEW_PALETTE_RGB:
            flat_palette.extend([r, g, b])
        flat_palette.extend([0, 0, 0] * (256 - len(COVER_PREVIEW_PALETTE_RGB)))
        palette_image.putpalette(flat_palette)

        with Image.open(cover_path) as image:
            image = image.convert("RGBA")
            alpha = image.getchannel("A")
            bbox = alpha.getbbox()
            if bbox:
                image = image.crop(bbox)

            src_w, src_h = image.size
            if src_w <= 0 or src_h <= 0:
                return None

            scale = min(COVER_PREVIEW_W / src_w, COVER_PREVIEW_H / src_h)
            out_w = max(1, int(round(src_w * scale)))
            out_h = max(1, int(round(src_h * scale)))
            image = image.resize((out_w, out_h), resample_lanczos)

            canvas = Image.new("RGBA", (COVER_PREVIEW_W, COVER_PREVIEW_H), COVER_PREVIEW_BG_RGB + (255,))
            off_x = (COVER_PREVIEW_W - out_w) // 2
            off_y = (COVER_PREVIEW_H - out_h) // 2
            canvas.alpha_composite(image, (off_x, off_y))

            quantized = canvas.convert("RGB").quantize(palette=palette_image, dither=dither_none)
            return quantized.tobytes()
    except Exception:
        return None


def stage_game(
    source_path: Path,
    context_dir: Path,
    build_root: Path,
    key_hint: str,
) -> tuple[Path, str, Path | None, str | None] | None:
    stage_dir = build_root / key_hint
    safe_rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    rom_stage = stage_dir / "rom.bin"
    rom_ext = ""
    direct_rom = find_rom_file(source_path if source_path.is_dir() else source_path)
    rom_source_name: str | None = None

    if direct_rom is not None:
        rom_ext = direct_rom.suffix.lower()
        rom_stage = stage_dir / f"rom{rom_ext}"
        shutil.copyfile(direct_rom, rom_stage)
        rom_source_name = direct_rom.name
    else:
        zip_candidates = [source_path] if source_path.is_file() else sorted(source_path.rglob("*.zip"))
        extracted = None
        for zip_path in zip_candidates:
            extracted = extract_rom_from_zip(zip_path, stage_dir / "rom")
            if extracted is not None:
                rom_source_name = extracted[0]
                rom_stage = extracted[1]
                rom_ext = extracted[2]
                break
        if extracted is None:
            safe_rmtree(stage_dir)
            return None

    cover_source = find_cover_file(context_dir, source_path, direct_rom)
    cover_stage = None
    if cover_source is not None:
        cover_stage = stage_dir / "cover.png"
        shutil.copyfile(cover_source, cover_stage)

    return rom_stage, rom_ext, cover_stage, rom_source_name


def discover_games(project_root: Path, stage_root: Path) -> list[GameAsset]:
    games: list[GameAsset] = []
    seen_hashes: set[str] = set()
    used_titles: dict[str, int] = {}

    for root in discover_auto_roots(project_root):
        if root.name in LIBRARY_ROOTS:
            packages = iter_library_sources(root)
        else:
            key_seed = slugify(root.stem if root.is_file() else root.name)
            packages = [
                PackageSource(
                    source_path=root,
                    context_dir=root if root.is_dir() else root.parent,
                    key_hint=key_seed,
                )
            ]
        for package in packages:
            staged = stage_game(package.source_path, package.context_dir, stage_root, package.key_hint)
            if staged is None:
                continue

            rom_stage, rom_ext, cover_stage, rom_source_name = staged
            rom_hash = hash_file(rom_stage)
            if rom_hash in seen_hashes:
                safe_rmtree(rom_stage.parent)
                continue
            seen_hashes.add(rom_hash)

            chosen_name = choose_base_name(package.context_dir, package.source_path, rom_source_name)
            title_base = normalize_title(chosen_name)
            count = used_titles.get(title_base, 0)
            used_titles[title_base] = count + 1
            title = title_base if count == 0 else f"{title_base}{count + 1}"
            games.append(
                GameAsset(
                    title=title,
                    rom_stage=rom_stage,
                    rom_ext=rom_ext,
                    cover_stage=cover_stage,
                    cover_preview=build_cover_preview(cover_stage),
                )
            )

    games.sort(key=lambda game: game.title.lower())
    return games


def make_path(path: Path) -> str:
    s = path.as_posix()
    m = re.match(r"^([A-Za-z]):/(.*)$", s)
    if not m:
        return s
    drive = m.group(1).lower()
    rest = m.group(2)
    return f"/mnt/{drive}/{rest}"


def symbol_name_for(path: Path) -> str:
    return "_binary_" + re.sub(r"[^A-Za-z0-9]", "_", make_path(path))


def preview_symbol_name(title: str) -> str:
    return "cover_preview_" + slugify(title)


def write_c_byte_array(lines: list[str], name: str, data: bytes, wrap: int = 24) -> None:
    lines.append(f"static const uint8_t {name}[{len(data)}] = {{")
    for i in range(0, len(data), wrap):
        chunk = ", ".join(str(v) for v in data[i:i + wrap])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")


def write_generated_c(out_c: Path, games: list[GameAsset]) -> None:
    lines: list[str] = []
    lines.append("/* Auto-generated by tools/gen_gba_assets.py. */")
    lines.append('#include "kernel/jack/jack_assets.h"')
    lines.append('#include "kernel/jack/vm_store.h"')
    lines.append('#include "kernel/lib/string.h"')
    lines.append("#include <stdint.h>")
    lines.append("")

    for game in games:
        rom_sym = symbol_name_for(game.rom_stage)
        lines.append(f"extern const uint8_t {rom_sym}_start[];")
        lines.append(f"extern const uint8_t {rom_sym}_end[];")
        if game.cover_stage is not None:
            cover_sym = symbol_name_for(game.cover_stage)
            lines.append(f"extern const uint8_t {cover_sym}_start[];")
            lines.append(f"extern const uint8_t {cover_sym}_end[];")
    lines.append("")

    lines.append("typedef struct {")
    lines.append("    const char *title;")
    lines.append("    const uint8_t *pixels;")
    lines.append("    uint16_t width;")
    lines.append("    uint16_t height;")
    lines.append("} JackAutoCoverPreview;")
    lines.append("")

    for game in games:
        if game.cover_preview is not None:
            write_c_byte_array(lines, preview_symbol_name(game.title), game.cover_preview)

    lines.append("static const JackAutoCoverPreview g_auto_cover_previews[] = {")
    for game in games:
        if game.cover_preview is None:
            continue
        lines.append(
            f'    {{"{game.title}", {preview_symbol_name(game.title)}, {COVER_PREVIEW_W}, {COVER_PREVIEW_H}}},'
        )
    lines.append("    {0, 0, 0, 0}")
    lines.append("};")
    lines.append("")

    lines.append("void jack_assets_auto_mount(void) {")
    if not games:
        lines.append("    (void)0;")
    for game in games:
        rom_sym = symbol_name_for(game.rom_stage)
        lines.append("    {")
        lines.append(f"        uint32_t size = (uint32_t)({rom_sym}_end - {rom_sym}_start);")
        lines.append("        if (size > 0) {")
        lines.append(
            f'            vm_store_add("{game.title}{game.rom_ext}", (const char *){rom_sym}_start, size);'
        )
        lines.append("        }")
        lines.append("    }")
        if game.cover_stage is not None:
            cover_sym = symbol_name_for(game.cover_stage)
            lines.append("    {")
            lines.append(
                f"        uint32_t size = (uint32_t)({cover_sym}_end - {cover_sym}_start);"
            )
            lines.append("        if (size > 0) {")
            lines.append(
                f'            vm_store_add("{game.title}/cover.png", (const char *){cover_sym}_start, size);'
            )
            lines.append("        }")
            lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("int jack_assets_get_generated_cover_preview(const char *title, const uint8_t **out_pixels, int *out_w, int *out_h) {")
    lines.append("    int i;")
    lines.append("    if (!title) return 0;")
    lines.append("    for (i = 0; g_auto_cover_previews[i].title; i++) {")
    lines.append("        if (kstrcmp(g_auto_cover_previews[i].title, title) == 0) {")
    lines.append("            if (out_pixels) *out_pixels = g_auto_cover_previews[i].pixels;")
    lines.append("            if (out_w) *out_w = (int)g_auto_cover_previews[i].width;")
    lines.append("            if (out_h) *out_h = (int)g_auto_cover_previews[i].height;")
    lines.append("            return 1;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return 0;")
    lines.append("}")
    lines.append("")

    out_c.parent.mkdir(parents=True, exist_ok=True)
    out_c.write_text("\n".join(lines), encoding="utf-8")


def write_generated_mk(out_mk: Path, games: list[GameAsset]) -> None:
    lines: list[str] = []
    obj_paths: list[str] = []
    for game in games:
        rom_obj = make_path(game.rom_stage.parent / "rom.o")
        obj_paths.append(rom_obj)
        if game.cover_stage is not None:
            obj_paths.append(make_path(game.cover_stage.parent / "cover.o"))

    lines.append("# Auto-generated by tools/gen_gba_assets.py.")
    lines.append("GBA_AUTO_OBJS := " + " ".join(obj_paths))
    lines.append("")

    for game in games:
        rom_src = make_path(game.rom_stage)
        rom_obj = make_path(game.rom_stage.parent / "rom.o")
        lines.append(f"{rom_obj}: {rom_src}")
        lines.append("\tmkdir -p $(dir $@)")
        lines.append("\t$(LD) -r -m elf_i386 -b binary -o $@ $<")
        lines.append("")
        if game.cover_stage is not None:
            cover_src = make_path(game.cover_stage)
            cover_obj = make_path(game.cover_stage.parent / "cover.o")
            lines.append(f"{cover_obj}: {cover_src}")
            lines.append("\tmkdir -p $(dir $@)")
            lines.append("\t$(LD) -r -m elf_i386 -b binary -o $@ $<")
            lines.append("")

    out_mk.parent.mkdir(parents=True, exist_ok=True)
    out_mk.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    global ROM_EXTENSIONS

    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", default=".")
    parser.add_argument("--out-c", required=True)
    parser.add_argument("--out-mk", required=True)
    parser.add_argument("--stage-dir", required=True)
    parser.add_argument("--rom-exts", default="")
    args = parser.parse_args()

    ROM_EXTENSIONS = parse_rom_exts(args.rom_exts)

    project_root = Path(args.project_root).resolve()
    out_c = Path(args.out_c)
    out_mk = Path(args.out_mk)
    stage_dir = Path(args.stage_dir)

    if not out_c.is_absolute():
        out_c = project_root / out_c
    if not out_mk.is_absolute():
        out_mk = project_root / out_mk
    if not stage_dir.is_absolute():
        stage_dir = project_root / stage_dir

    safe_rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    games = discover_games(project_root, stage_dir)
    write_generated_c(out_c, games)
    write_generated_mk(out_mk, games)

    print(f"[gen_gba_assets] {len(games)} ROM(s) embarquee(s)")
    for game in games:
        print(f"  - {game.title}{game.rom_ext}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
