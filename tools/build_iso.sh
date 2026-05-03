#!/usr/bin/env bash
set -euo pipefail

ISO_OUT="${1:-alos.iso}"
KERNEL_ELF="${2:-alos.elf}"
GRUB_CFG="${3:-grub/grub.cfg}"
INSTALL_IMG="${4:-}"
ISO_DIR="build/isodir"

if ! command -v grub-mkrescue >/dev/null 2>&1; then
    echo "Erreur: grub-mkrescue introuvable. Installe grub-pc-bin + xorriso." >&2
    exit 1
fi

if [[ ! -f "${KERNEL_ELF}" ]]; then
    echo "Erreur: kernel ELF introuvable: ${KERNEL_ELF}" >&2
    exit 1
fi

if [[ ! -f "${GRUB_CFG}" ]]; then
    echo "Erreur: grub.cfg introuvable: ${GRUB_CFG}" >&2
    exit 1
fi

rm -rf "${ISO_DIR}"
mkdir -p "${ISO_DIR}/boot/grub"

cp "${KERNEL_ELF}" "${ISO_DIR}/boot/alos.elf"
cp "${GRUB_CFG}" "${ISO_DIR}/boot/grub/grub.cfg"
if [[ -n "${INSTALL_IMG}" ]]; then
    if [[ ! -f "${INSTALL_IMG}" ]]; then
        echo "Erreur: image installateur introuvable: ${INSTALL_IMG}" >&2
        exit 1
    fi
    cp "${INSTALL_IMG}" "${ISO_DIR}/boot/alos_target.img"
fi

grub-mkrescue -o "${ISO_OUT}" "${ISO_DIR}" >/dev/null
echo "ISO bootable generee: ${ISO_OUT}"
