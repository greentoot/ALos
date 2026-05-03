#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <melonds-src-dir> <melonds-build32-dir> <out-dir>" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
abs_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s/%s\n' "$ROOT_DIR" "$1" ;;
    esac
}

SRC_DIR="$(abs_path "$1")"
BUILD_DIR="$(abs_path "$2")"
OUT_DIR="$(abs_path "$3")"

cd "$ROOT_DIR"

CORE_LIB="$BUILD_DIR/src/libcore.a"
TEAKRA_LIB="$BUILD_DIR/src/teakra/src/libteakra.a"
ALOS_OBJS=(
    "$ROOT_DIR/kernel/lib/libc_compat.o"
    "$ROOT_DIR/kernel/lib/string.o"
    "$ROOT_DIR/kernel/lib/kprintf.o"
    "$ROOT_DIR/kernel/memory/heap.o"
    "$ROOT_DIR/kernel/fs/ramfs.o"
    "$ROOT_DIR/kernel/fs/persist.o"
    "$ROOT_DIR/kernel/jack/vm_store.o"
    "$ROOT_DIR/driver/timer.o"
)
LIBSTDCXX="$(g++ -m32 -print-file-name=libstdc++.a)"
EXTRA_LINK_OBJS=()
EXTRA_LINK_LIBS=()
USE_HOST_STDLIB=0

mkdir -p "$OUT_DIR"

for obj in "${ALOS_OBJS[@]}"; do
    if [ -f "$obj" ]; then
        EXTRA_LINK_OBJS+=("$obj")
    fi
done
if [ -f "$TEAKRA_LIB" ]; then
    EXTRA_LINK_LIBS+=("$TEAKRA_LIB")
fi
if [ -f "$LIBSTDCXX" ]; then
    EXTRA_LINK_LIBS+=("$LIBSTDCXX")
    USE_HOST_STDLIB=1
fi
COMMON_FLAGS=(
    -m32
    -std=gnu++17
    -fno-builtin
    -fno-stack-protector
    -fno-pie
    -fno-pic
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -Wall
    -Wextra
    -O2
    -DMELONDS_DS_RETAIL_ONLY=1
    -I"$ROOT_DIR"
    -I"$SRC_DIR/src"
    -I"$BUILD_DIR/src"
    -I"$SRC_DIR/src/teakra/include"
)

if [ "$USE_HOST_STDLIB" -eq 0 ]; then
    g++ "${COMMON_FLAGS[@]}" -c "$ROOT_DIR/kernel/lib/cxx_runtime.cpp" -o "$OUT_DIR/cxx_runtime.o"
fi
g++ "${COMMON_FLAGS[@]}" -c "$ROOT_DIR/kernel/nds/melonds_platform_alos.cpp" -o "$OUT_DIR/melonds_platform_alos.o"
g++ "${COMMON_FLAGS[@]}" -c "$ROOT_DIR/kernel/nds/nds_core_melonds.cpp" -o "$OUT_DIR/nds_core_melonds.o"

cat >"$OUT_DIR/probe.cpp" <<'EOF'
#include "third_party/melonds/src/NDS.h"
extern "C" int alos_melonds_internal_probe_symbol(void) {
    melonDS::NDS *nds = nullptr;
    return nds ? 1 : 0;
}
EOF

g++ "${COMMON_FLAGS[@]}" -c "$OUT_DIR/probe.cpp" -o "$OUT_DIR/probe.o"

LINK_LOG="$OUT_DIR/link_attempt.txt"
UNRESOLVED_LOG="$OUT_DIR/unresolved_symbols.txt"
LINK_INPUTS=()

if [ "$USE_HOST_STDLIB" -eq 0 ]; then
    LINK_INPUTS+=("$OUT_DIR/cxx_runtime.o")
fi
LINK_INPUTS+=(
    "$OUT_DIR/melonds_platform_alos.o"
    "$OUT_DIR/nds_core_melonds.o"
    "$OUT_DIR/probe.o"
)

if ld -m elf_i386 -r \
    "${LINK_INPUTS[@]}" \
    "${EXTRA_LINK_OBJS[@]}" \
    --whole-archive \
    "$CORE_LIB" "${EXTRA_LINK_LIBS[@]}" \
    --no-whole-archive \
    -o "$OUT_DIR/melonds_internal_probe.o" \
    >"$LINK_LOG" 2>&1; then
    nm -u "$OUT_DIR/melonds_internal_probe.o" | c++filt | sort | uniq >"$UNRESOLVED_LOG" || true
    echo "Probe interne generee: $OUT_DIR/melonds_internal_probe.o"
else
    {
        echo "La liaison relocatable complete n'a pas abouti."
        echo
        echo "Dernier log ld:"
        cat "$LINK_LOG"
        echo
        echo "Symboles externes du core melonDS 32-bit:"
        nm -u "$CORE_LIB" | c++filt | sort | uniq
    } >"$UNRESOLVED_LOG" || true
    echo "Probe interne compilee, rapport genere: $UNRESOLVED_LOG"
fi
