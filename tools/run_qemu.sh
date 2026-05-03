#!/usr/bin/env bash
set -u

QEMU_BIN="$1"
shift
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENABLE_HOST_BRIDGE="${ALOS_ENABLE_HOST_BRIDGE:-0}"

LOG_DIR="${TMPDIR:-/tmp}"
ERR_LOG="${LOG_DIR}/alos-qemu-stderr.$$"
SERIAL_SOCK="${LOG_DIR}/alos-qemu-serial.$$.sock"
HOST_BRIDGE_PID=""

cleanup() {
  rm -f "${ERR_LOG}"
  rm -f "${SERIAL_SOCK}"
  if [ -n "${HOST_BRIDGE_PID}" ]; then
    kill "${HOST_BRIDGE_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

start_host_bridge() {
  rm -f "${SERIAL_SOCK}"
  python3 "${ROOT_DIR}/tools/serial_host_bridge.py" --socket "${SERIAL_SOCK}" --root "${ROOT_DIR}" >/dev/null 2>&1 &
  HOST_BRIDGE_PID=$!
}

run_once() {
  local mode="$1"
  shift
  local serial_args=()
  if [ "${ENABLE_HOST_BRIDGE}" = "1" ]; then
    start_host_bridge
    serial_args=(
      -chardev "socket,id=alosserial,path=${SERIAL_SOCK},server=on,wait=off"
      -serial chardev:alosserial
    )
    echo "[ALOS] Mode QEMU: bridge host DEV active"
  else
    echo "[ALOS] Mode QEMU: portable pur (sans bridge host)"
  fi
  echo "[ALOS] Lancement QEMU (${mode})"
  "${QEMU_BIN}" \
    "${serial_args[@]}" \
    "$@" 2> >(tee "${ERR_LOG}" >&2)
  local rc=$?
  if [ -n "${HOST_BRIDGE_PID}" ]; then
    kill "${HOST_BRIDGE_PID}" >/dev/null 2>&1 || true
    wait "${HOST_BRIDGE_PID}" >/dev/null 2>&1 || true
    HOST_BRIDGE_PID=""
  fi
  rm -f "${SERIAL_SOCK}"
  return "${rc}"
}

run_once "persist" "$@"
rc=$?
if [ "${rc}" -eq 0 ]; then
  exit 0
fi

if [ "${rc}" -eq 134 ] && grep -q "qemu_mutex_lock_iothread_impl" "${ERR_LOG}"; then
  echo "[ALOS] QEMU 8.2.2 a plante sur l'assertion iothread."
  echo "[ALOS] Fallback automatique: relance sans disque persistant."
  filtered=()
  skip_next=0
  for arg in "$@"; do
    if [ "${skip_next}" -eq 1 ]; then
      skip_next=0
      continue
    fi
    if [ "${arg}" = "-drive" ]; then
      skip_next=1
      continue
    fi
    filtered+=("${arg}")
  done
  run_once "sans-persist" "${filtered[@]}"
  exit $?
fi

exit "${rc}"
