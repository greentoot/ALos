#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <melonds-src-dir> <melonds-build32-dir>" >&2
    exit 1
fi

SRC_DIR="$1"
BUILD_DIR="$2"

cmake \
    -S "$SRC_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_QT_SDL=OFF \
    -DENABLE_OGLRENDERER=OFF \
    -DENABLE_JIT=OFF \
    -DENABLE_GDBSTUB=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_LTO_RELEASE=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
    -DMELONDS_DS_RETAIL_ONLY=ON \
    -DCMAKE_C_FLAGS="-m32 -O3 -fomit-frame-pointer -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" \
    -DCMAKE_CXX_FLAGS="-m32 -O3 -fomit-frame-pointer -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" \
    >/dev/null

cmake --build "$BUILD_DIR" -j2 >/dev/null

echo "melonDS core 32-bit genere: $BUILD_DIR/src/libcore.a"
