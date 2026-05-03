#!/usr/bin/env bash
set -euo pipefail

IMG_OUT="${1:-alos.img}"
KERNEL_ELF="${2:-alos.elf}"
GRUB_CFG="${3:-grub/grub.cfg}"
IMG_MB="${4:-128}"
MNT_DIR="build/mnt"

for cmd in dd parted sfdisk losetup mkfs.ext2 mount umount grub-install; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Erreur: commande manquante: ${cmd}" >&2
        exit 1
    fi
done

if [[ ! -f "${KERNEL_ELF}" ]]; then
    echo "Erreur: kernel ELF introuvable: ${KERNEL_ELF}" >&2
    exit 1
fi

if [[ ! -f "${GRUB_CFG}" ]]; then
    echo "Erreur: grub.cfg introuvable: ${GRUB_CFG}" >&2
    exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        exec sudo "$0" "$@"
    fi
    echo "Erreur: droits root requis (sudo indisponible)." >&2
    exit 1
fi

mkdir -p build
rm -f "${IMG_OUT}"
dd if=/dev/zero of="${IMG_OUT}" bs=1M count="${IMG_MB}" status=none

parted -s "${IMG_OUT}" mklabel msdos
parted -s "${IMG_OUT}" mkpart primary ext2 1MiB -32MiB
parted -s "${IMG_OUT}" mkpart primary -32MiB 100%
parted -s "${IMG_OUT}" set 1 boot on
# Partition 2 = ALOS persistence raw area (custom type 0xA0)
sfdisk --part-type "${IMG_OUT}" 2 a0 >/dev/null

LOOP_DEV=""
cleanup() {
    set +e
    if mountpoint -q "${MNT_DIR}"; then
        umount "${MNT_DIR}"
    fi
    if [[ -n "${LOOP_DEV}" ]]; then
        losetup -d "${LOOP_DEV}"
    fi
}
trap cleanup EXIT

LOOP_DEV="$(losetup --find --show --partscan "${IMG_OUT}")"
PART_DEV="${LOOP_DEV}p1"

if [[ ! -b "${PART_DEV}" ]]; then
    partprobe "${LOOP_DEV}" || true
    sleep 1
fi

if [[ ! -b "${PART_DEV}" ]]; then
    echo "Erreur: partition loop non detectee (${PART_DEV})." >&2
    exit 1
fi

mkfs.ext2 -F "${PART_DEV}" >/dev/null
mkdir -p "${MNT_DIR}"
mount "${PART_DEV}" "${MNT_DIR}"

mkdir -p "${MNT_DIR}/boot/grub"
cp "${KERNEL_ELF}" "${MNT_DIR}/boot/alos.elf"
cp "${GRUB_CFG}" "${MNT_DIR}/boot/grub/grub.cfg"

grub-install \
    --target=i386-pc \
    --boot-directory="${MNT_DIR}/boot" \
    --modules="part_msdos ext2 multiboot normal biosdisk" \
    --no-floppy \
    --recheck \
    "${LOOP_DEV}" >/dev/null

sync
echo "Image disque bootable generee: ${IMG_OUT}"
