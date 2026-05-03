#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="${1:-third_party/melonds}"
BUILD_DIR="${2:-${SRC_DIR}/build-alos-core}"
REPO_URL="${MELONDS_REPO_URL:-https://github.com/melonDS-emu/melonDS}"

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[melonDS] Outil requis manquant: $1" >&2
        exit 1
    }
}

need_cmd git
need_cmd cmake
need_cmd g++

if [[ ! -d "$SRC_DIR" ]]; then
    echo "[melonDS] Clone officiel dans $SRC_DIR"
    git clone --depth 1 "$REPO_URL" "$SRC_DIR"
else
    echo "[melonDS] Checkout existant detecte: $SRC_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "[melonDS] Configuration coeur minimal (sans frontend Qt/SDL)..."
if ! cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DBUILD_QT_SDL=OFF \
    -DENABLE_OGLRENDERER=OFF \
    -DENABLE_JIT=OFF \
    -DENABLE_GDBSTUB=OFF >/tmp/alos-melonds-cmake.log 2>&1; then
    cat /tmp/alos-melonds-cmake.log >&2
    echo "[melonDS] Echec configuration. Cible valide testee: BUILD_QT_SDL=OFF, ENABLE_OGLRENDERER=OFF, ENABLE_JIT=OFF, ENABLE_GDBSTUB=OFF" >&2
    exit 1
fi

echo "[melonDS] Build du core..."
if ! cmake --build "$BUILD_DIR" -j"$(nproc)" >/tmp/alos-melonds-build.log 2>&1; then
    cat /tmp/alos-melonds-build.log >&2
    echo "[melonDS] Echec build. Le coeur officiel reste optionnel et n'est pas encore lie au kernel." >&2
    exit 1
fi

if [[ ! -f "$BUILD_DIR/src/libcore.a" ]]; then
    echo "[melonDS] libcore.a introuvable apres build." >&2
    exit 1
fi

echo "[melonDS] OK: $BUILD_DIR/src/libcore.a"
