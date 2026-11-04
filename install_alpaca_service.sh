#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
# Udev rules, vendor libraries and the dialout/gpio groups, shared with build_and_run.sh (#1698).
# shellcheck source=scripts/host_setup.sh
source "${ROOT_DIR}/scripts/host_setup.sh"
SERVICE_NAME="alpacahttp.service"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
INSTALL_UDEV_RULES="${ALPACA_INSTALL_UDEV_RULES:-ON}"
ADD_DIALOUT="${ALPACA_ADD_DIALOUT:-ON}"
ADD_GPIO="${ALPACA_ADD_GPIO:-ON}"
GIT_PULL="${ALPACA_GIT_PULL:-OFF}"
CLEAN_BUILD="${ALPACA_CLEAN_BUILD:-OFF}"

usage() {
  cat <<'USAGE'
Usage: install_alpaca_service.sh <install|update|uninstall|status>

Environment overrides:
  ALPACACORE_ENABLE_ALL_VENDORS=ON|OFF
  ALPACA_INSTALL_UDEV_RULES=ON|OFF
  ALPACA_ADD_DIALOUT=ON|OFF       (add the user to dialout: serial devices)
  ALPACA_ADD_GPIO=ON|OFF          (create the gpio group, add the user: ASIAIR power ports)
  ALPACA_GIT_PULL=ON|OFF          (update only)
  ALPACA_CLEAN_BUILD=ON|OFF       (install/update)
USAGE
}

ensure_repo_paths() {
  if [[ ! -d "${CORE_DIR}" ]]; then
    echo "AlpacaCore not found at ${CORE_DIR}"
    exit 1
  fi
  if [[ ! -d "${HTTP_DIR}" ]]; then
    echo "AlpacaHTTP not found at ${HTTP_DIR}"
    exit 1
  fi
}

require_linux() {
  if [[ "${OSTYPE:-}" != "linux"* ]]; then
    echo "This script is intended for Linux (arm64)."
    exit 1
  fi
  local arch
  arch="$(uname -m)"
  if [[ "${arch}" != "aarch64" && "${arch}" != "arm64" ]]; then
    echo "AlpacaBridge supports Linux arm64 only. Detected: ${arch}"
    exit 1
  fi
}

detect_parallel() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  else
    echo "4"
  fi
}

install_udev_rules() {
  if [[ "${INSTALL_UDEV_RULES}" != "ON" ]]; then
    return
  fi
  host_setup_install_vendor_runtime "${CORE_DIR}"
}

# The service runs as this user (write_service_unit), so it needs the same groups build_and_run.sh gives
# it. Runs on every install and update; each step is a no-op when already done.
ensure_device_groups() {
  host_setup_ensure_dialout "${ADD_DIALOUT}"
  host_setup_ensure_gpio "${ADD_GPIO}"
}

clean_build_dir() {
  local build_dir="$1"
  if [[ -d "${build_dir}" ]]; then
    rm -rf "${build_dir}"
  fi
}

# libgphoto2 is bundled: the gphoto driver links the copy that
# scripts/build_bundled_gphoto.sh builds from the vendored tarball into
# /usr/lib/alpacabridge/libgphoto2 (the distro's libgphoto2 is not used). Build it
# when it is missing or at another version; otherwise skip. Keep this function
# identical in build_and_run.sh and install_alpaca_service.sh.
ensure_bundled_gphoto() {
  [[ "${CORE_VENDORS}" == "ON" && "${OSTYPE:-}" == "linux"* ]] || return 0
  local pins LIBGPHOTO2_VER pc
  pins="$("${ROOT_DIR}/scripts/build_bundled_gphoto.sh" --print-pins)"
  LIBGPHOTO2_VER="$(sed -n 's/^LIBGPHOTO2_VER=//p' <<<"${pins}")"
  pc="/usr/lib/alpacabridge/libgphoto2/lib/pkgconfig/libgphoto2.pc"
  if [[ "$(sed -n 's/^Version: //p' "${pc}" 2>/dev/null)" == "${LIBGPHOTO2_VER}" ]]; then
    return 0
  fi
  echo "Building bundled libgphoto2 ${LIBGPHOTO2_VER}"
  sudo "${ROOT_DIR}/scripts/build_bundled_gphoto.sh"
  sudo udevadm control --reload-rules
  sudo udevadm trigger
}

build_projects() {
  ensure_bundled_gphoto

  local parallel
  parallel="$(detect_parallel)"

  if [[ "${CLEAN_BUILD}" == "ON" ]]; then
    clean_build_dir "${CORE_DIR}/build"
    clean_build_dir "${HTTP_DIR}/build"
  fi

  echo "== AlpacaCore =="
  cmake -S "${CORE_DIR}" -B "${CORE_DIR}/build" \
    -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
  cmake --build "${CORE_DIR}/build" --parallel "${parallel}"

  echo "== AlpacaHTTP =="
  cmake -S "${HTTP_DIR}" -B "${HTTP_DIR}/build" \
    -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
  cmake --build "${HTTP_DIR}/build" --parallel "${parallel}"
}

write_service_unit() {
  local user group
  user="$(id -un)"
  group="$(id -gn)"

  sudo tee "/etc/systemd/system/${SERVICE_NAME}" >/dev/null <<EOF_UNIT
[Unit]
Description=AlpacaHTTP Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${user}
Group=${group}
WorkingDirectory=${HTTP_DIR}
ExecStart=${HTTP_DIR}/build/alpacahttp_server
LogsDirectory=AlpacaBridge
LogsDirectoryMode=0755
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF_UNIT

  # Ensure the log directory exists with correct ownership even if the binary
  # is run outside systemd (e.g., manual invocation during development).
  if [[ ! -d /var/log/AlpacaBridge ]]; then
    sudo mkdir -p /var/log/AlpacaBridge
  fi
  sudo chown "${user}:${group}" /var/log/AlpacaBridge
  sudo chmod 0755 /var/log/AlpacaBridge

  sudo systemctl daemon-reload
}

start_service() {
  sudo systemctl enable "${SERVICE_NAME}"
  sudo systemctl restart "${SERVICE_NAME}"
}

stop_service() {
  if systemctl is-active --quiet "${SERVICE_NAME}"; then
    sudo systemctl stop "${SERVICE_NAME}"
  fi
}

case "${1:-}" in
  install)
    require_linux
    ensure_repo_paths
    install_udev_rules
    ensure_device_groups
    build_projects
    write_service_unit
    start_service
    ;;
  update)
    require_linux
    ensure_repo_paths
    if [[ "${GIT_PULL}" == "ON" ]]; then
      git -C "${ROOT_DIR}" pull
    fi
    stop_service
    install_udev_rules
    ensure_device_groups
    build_projects
    # Refresh the unit file so update installs pick up service-unit changes
    # (e.g. new LogsDirectory directives) and re-assert log directory ownership.
    write_service_unit
    start_service
    ;;
  uninstall)
    require_linux
    stop_service
    if systemctl is-enabled --quiet "${SERVICE_NAME}"; then
      sudo systemctl disable "${SERVICE_NAME}"
    fi
    if [[ -f "/etc/systemd/system/${SERVICE_NAME}" ]]; then
      sudo rm -f "/etc/systemd/system/${SERVICE_NAME}"
      sudo systemctl daemon-reload
    fi
    ;;
  status)
    require_linux
    systemctl status "${SERVICE_NAME}" --no-pager
    ;;
  *)
    usage
    exit 1
    ;;
esac
