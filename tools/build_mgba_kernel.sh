#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MGBADIR="${ROOT_DIR}/third_party/mgba"
BUILDDIR="${MGBADIR}/build-alos-kernel2"

rm -rf "${BUILDDIR}"
mkdir -p "${BUILDDIR}"
cd "${BUILDDIR}"

export CFLAGS="-m32 -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-pic -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0"
export CXXFLAGS="-m32"

cmake "${MGBADIR}" \
  -DBUILD_LTO=OFF \
  -DLIBMGBA_ONLY=ON \
  -DM_CORE_GBA=ON \
  -DM_CORE_GB=ON \
  -DDISABLE_FRONTENDS=ON \
  -DDISABLE_DEPS=ON \
  -DBUILD_STATIC=ON \
  -DBUILD_SHARED=OFF \
  -DMINIMAL_CORE=ON \
  -DENABLE_VFS=OFF \
  -DUSE_PTHREADS=OFF \
  -DUSE_ELF=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build . -j"$(nproc)"
echo "[OK] libmgba built at ${BUILDDIR}/libmgba.a"
