#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
HTTP_BEAST="${ALPACAHTTP_USE_BOOST_BEAST:-OFF}"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
INSTALL_UDEV_RULES="${ALPACA_INSTALL_UDEV_RULES:-ON}"

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi

if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

rm -rf "${CORE_DIR}/build" "${HTTP_DIR}/build"

if [[ "${INSTALL_UDEV_RULES}" == "ON" && "${OSTYPE:-}" == "linux"* ]]; then
  RULES_SRC=()
  while IFS= read -r -d '' rule; do
    RULES_SRC+=("${rule}")
  done < <(find "${CORE_DIR}/external" -name "*.rules" -type f -print0 | sort -z)
  for rule in "${RULES_SRC[@]}"; do
    echo "Installing udev rule: ${rule}"
    sudo install -m 644 "${rule}" /etc/udev/rules.d/
  done
  sudo udevadm control --reload-rules
  sudo udevadm trigger
fi

if [[ "${OSTYPE:-}" == "darwin"* ]]; then
  PARALLEL="$(sysctl -n hw.ncpu)"
else
  if command -v nproc >/dev/null 2>&1; then
    PARALLEL="$(nproc)"
  else
    PARALLEL="4"
  fi
fi

build_make() {
  local project_dir="$1"
  local name="$2"

  echo "== ${name} =="
  if [[ "${name}" == "AlpacaHTTP" ]]; then
    cmake -S "${project_dir}" -B "${project_dir}/build" \
      -DALPACAHTTP_USE_BOOST_BEAST="${HTTP_BEAST}" \
      -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
  else
    cmake -S "${project_dir}" -B "${project_dir}/build" \
      -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
  fi
  if [[ -f "${project_dir}/build/Makefile" ]]; then
    make -C "${project_dir}/build" clean 2>&1
    make -C "${project_dir}/build" -j"${PARALLEL}" 2>&1
  else
    cmake --build "${project_dir}/build" --parallel "${PARALLEL}" 2>&1
  fi
}

build_make "${CORE_DIR}" "AlpacaCore"
build_make "${HTTP_DIR}" "AlpacaHTTP"

SERVER_BIN="${HTTP_DIR}/build/alpacahttp_server"
if [[ -x "${SERVER_BIN}" ]]; then
  echo "AlpacaHTTP is running. Open http://localhost:6800/ in your browser."
  exec "${SERVER_BIN}"
fi

if [[ -x "${HTTP_DIR}/build/Debug/alpacahttp_server" ]]; then
  echo "AlpacaHTTP is running. Open http://localhost:6800/ in your browser."
  exec "${HTTP_DIR}/build/Debug/alpacahttp_server"
fi

if [[ -x "${HTTP_DIR}/build/Release/alpacahttp_server" ]]; then
  echo "AlpacaHTTP is running. Open http://localhost:6800/ in your browser."
  exec "${HTTP_DIR}/build/Release/alpacahttp_server"
fi

echo "Could not find alpacahttp_server binary in ${HTTP_DIR}/build"
exit 1
