#!/usr/bin/env bash
set -euo pipefail

IMG_OUT="${1:-alos_persist.img}"
IMG_MB="${2:-64}"

for cmd in dd sfdisk; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Erreur: commande manquante: ${cmd}" >&2
        exit 1
    fi
done

mkdir -p "$(dirname "${IMG_OUT}")" 2>/dev/null || true
if [[ -f "${IMG_OUT}" ]]; then
    touch "${IMG_OUT}"
    echo "Image persistance deja presente: ${IMG_OUT} (conservee)"
    exit 0
fi

dd if=/dev/zero of="${IMG_OUT}" bs=1M count="${IMG_MB}" status=none

cat <<EOF | sfdisk "${IMG_OUT}" >/dev/null
label: dos
unit: sectors

2048,,a0
EOF

echo "Image persistance generee: ${IMG_OUT} (${IMG_MB} MiB, partition type 0xA0)"
