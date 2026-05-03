#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:?project root missing}"
REQUEST_ID="${2:?request id missing}"
ROM_NAME="${3:?rom name missing}"

VIEWER_PY="${ROOT_DIR}/tools/melonds_live.py"
RUNNER_LINUX="${ROOT_DIR}/build/melonds_min/melonds_runner"

find_rom() {
  find "${ROOT_DIR}/rom_library" "${ROOT_DIR}/gba_library" -type f -name "${ROM_NAME}" 2>/dev/null | head -n 1
}

ROM_LINUX="$(find_rom || true)"
if [ -z "${ROM_LINUX}" ]; then
  echo "[ALOS] Bridge DS: ROM introuvable pour ${ROM_NAME}" >&2
  exit 1
fi

if [ ! -x "${RUNNER_LINUX}" ]; then
  (cd "${ROOT_DIR}" && make melonds-runner >/dev/null)
fi

if ! command -v python.exe >/dev/null 2>&1; then
  echo "[ALOS] Bridge DS: python.exe introuvable pour le viewer live." >&2
  exit 1
fi

IPC_LINUX="${ROOT_DIR}/build/melonds_live/${REQUEST_ID}"
mkdir -p "${IPC_LINUX}"

ROOT_WIN="$(wslpath -w "${ROOT_DIR}")"
ROM_WIN="$(wslpath -w "${ROM_LINUX}")"
VIEWER_WIN="$(wslpath -w "${VIEWER_PY}")"
IPC_WIN="$(wslpath -w "${IPC_LINUX}")"

python.exe "${VIEWER_WIN}" \
  --project-root "${ROOT_WIN}" \
  --project-root-wsl "${ROOT_DIR}" \
  --rom "${ROM_WIN}" \
  --rom-wsl "${ROM_LINUX}" \
  --runner-wsl "${RUNNER_LINUX}" \
  --ipc-dir "${IPC_WIN}" \
  --ipc-dir-wsl "${IPC_LINUX}" \
  >/dev/null 2>&1
