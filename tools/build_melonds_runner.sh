#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <melonds-src-dir> <melonds-build-dir> <output-bin>" >&2
    exit 1
fi

SRC_DIR="$1"
BUILD_DIR="$2"
OUT_BIN="$3"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER_DIR="$ROOT_DIR/host/melonds_min"
TEAKRA_LIB="$BUILD_DIR/src/teakra/src/libteakra.a"
CORE_LIB="$BUILD_DIR/src/libcore.a"

if [ ! -f "$CORE_LIB" ]; then
    echo "Erreur: core melonDS absent: $CORE_LIB" >&2
    echo "Lance d'abord: make melonds-core" >&2
    exit 1
fi

if [ ! -f "$TEAKRA_LIB" ]; then
    echo "Erreur: libteakra absente: $TEAKRA_LIB" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT_BIN")"

g++ \
    -O2 \
    -std=gnu++17 \
    -DARCHITECTURE_x86_64=1 \
    -I"$SRC_DIR/src" \
    -I"$BUILD_DIR/src" \
    -I"$SRC_DIR/src/teakra/include" \
    "$RUNNER_DIR/platform_minimal.cpp" \
    "$RUNNER_DIR/runner.cpp" \
    "$CORE_LIB" \
    "$TEAKRA_LIB" \
    -pthread \
    -ldl \
    -o "$OUT_BIN"

echo "Runner melonDS genere: $OUT_BIN"
