#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
SERVICE_NAME="alpacahttp.service"
HTTP_BEAST="${ALPACAHTTP_USE_BOOST_BEAST:-OFF}"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
INSTALL_UDEV_RULES="${ALPACA_INSTALL_UDEV_RULES:-ON}"
GIT_PULL="${ALPACA_GIT_PULL:-OFF}"
CLEAN_BUILD="${ALPACA_CLEAN_BUILD:-OFF}"

usage() {
  cat <<'USAGE'
Usage: install_alpaca_service.sh <install|update|uninstall|status>

Environment overrides:
  ALPACAHTTP_USE_BOOST_BEAST=ON|OFF
  ALPACACORE_ENABLE_ALL_VENDORS=ON|OFF
  ALPACA_INSTALL_UDEV_RULES=ON|OFF
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
}

detect_parallel() {
  if command -v nproc >/dev/null 2>&1; then
    echo "$(nproc)"
  else
    echo "4"
  fi
}

install_udev_rules() {
  if [[ "${INSTALL_UDEV_RULES}" != "ON" ]]; then
    return
  fi

  # Deduplicate rules by basename so multiple copies of the same file (e.g.
  # the three copies of 85-qhyccd.rules inside the QHY SDK tree) only install
  # once — matching the single-file-per-vendor pattern used by ZWO.
  declare -A seen_rules
  RULES_SRC=()
  while IFS= read -r -d '' rule; do
    base="$(basename "${rule}")"
    if [[ -z "${seen_rules[${base}]+_}" ]]; then
      seen_rules["${base}"]=1
      RULES_SRC+=("${rule}")
    fi
  done < <(find "${CORE_DIR}/external" -name "*.rules" -type f -print0 | sort -z)

  if [[ ${#RULES_SRC[@]} -eq 0 ]]; then
    echo "No udev rules found under ${CORE_DIR}/external"
  else
    for rule in "${RULES_SRC[@]}"; do
      echo "Installing udev rule: ${rule}"
      sudo install -m 644 "${rule}" /etc/udev/rules.d/
    done
  fi

  # QHY cameras require firmware files that the udev rules load via fxload.
  # Detect architecture and install from the matching SDK directory.
  local arch
  arch="$(uname -m)"
  local qhy_sdk_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    qhy_sdk_dir="${CORE_DIR}/external/QHY/sdk_Arm64_25.09.29"
  elif [[ "${arch}" == "x86_64" ]]; then
    qhy_sdk_dir="${CORE_DIR}/external/QHY/sdk_linux64_25.09.29"
  fi

  if [[ -n "${qhy_sdk_dir}" && -d "${qhy_sdk_dir}/lib/firmware/qhy" ]]; then
    echo "Installing QHY firmware files from ${qhy_sdk_dir}/lib/firmware/qhy"
    sudo mkdir -p /lib/firmware/qhy
    sudo cp -a "${qhy_sdk_dir}/lib/firmware/qhy/." /lib/firmware/qhy/

    # The system fxload (from apt) does not support -t fx3 (FX3-based cameras).
    # The QHY SDK ships its own fxload binary that does. Install it so the
    # udev rule RUN+="/sbin/fxload -t fx3 ..." works correctly.
    if [[ -f "${qhy_sdk_dir}/sbin/fxload" ]]; then
      echo "Installing QHY SDK fxload (FX3-capable) to /sbin/fxload"
      sudo install -m 755 "${qhy_sdk_dir}/sbin/fxload" /sbin/fxload
    fi

    # Install the QHY shared library so the server can find it at runtime.
    if [[ -d "${qhy_sdk_dir}/usr/local/lib" ]]; then
      echo "Installing QHY shared libraries to /usr/local/lib"
      sudo cp -a "${qhy_sdk_dir}/usr/local/lib/libqhyccd.so"* /usr/local/lib/
      sudo ldconfig
    fi
  fi

  # Install ZWO ASI Camera shared library (used by SmartGuider via zwoasi)
  local zwo_camera_lib_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    zwo_camera_lib_dir="${CORE_DIR}/external/ZWO/ASI_Camera_SDK/lib/linux/armv8"
  elif [[ "${arch}" == "x86_64" ]]; then
    zwo_camera_lib_dir="${CORE_DIR}/external/ZWO/ASI_Camera_SDK/lib/linux/x64"
  fi
  if [[ -n "${zwo_camera_lib_dir}" && -f "${zwo_camera_lib_dir}/libASICamera2.so" ]]; then
    echo "Installing ZWO ASI Camera shared library to /usr/local/lib"
    sudo cp -a "${zwo_camera_lib_dir}/libASICamera2.so"* /usr/local/lib/
    sudo ldconfig
  fi

  # Install SVBONY Camera shared library
  local svbony_lib_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    svbony_lib_dir="${CORE_DIR}/external/SVBONY/lib/armv8"
  elif [[ "${arch}" == "x86_64" ]]; then
    svbony_lib_dir="${CORE_DIR}/external/SVBONY/lib/x64"
  fi
  if [[ -n "${svbony_lib_dir}" && -f "${svbony_lib_dir}/libSVBCameraSDK.so" ]]; then
    echo "Installing SVBONY Camera shared library to /usr/local/lib"
    sudo cp -a "${svbony_lib_dir}/libSVBCameraSDK.so"* /usr/local/lib/
    sudo ldconfig
  fi

  # Install ToupTek Camera shared library (used by companion guiding software)
  local touptek_lib_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    touptek_lib_dir="${CORE_DIR}/external/ToupTek/toupcamsdk.20260128/linux/arm64/glibc"
  elif [[ "${arch}" == "x86_64" ]]; then
    touptek_lib_dir="${CORE_DIR}/external/ToupTek/toupcamsdk.20260128/linux/x64"
  fi
  if [[ -n "${touptek_lib_dir}" && -f "${touptek_lib_dir}/libtoupcam.so" ]]; then
    echo "Installing ToupTek Camera shared library to /usr/local/lib"
    sudo cp -a "${touptek_lib_dir}/libtoupcam.so"* /usr/local/lib/
    sudo ldconfig
  fi

  sudo udevadm control --reload-rules
  sudo udevadm trigger
}

clean_build_dir() {
  local build_dir="$1"
  if [[ -d "${build_dir}" ]]; then
    rm -rf "${build_dir}"
  fi
}

build_projects() {
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
    -DALPACAHTTP_USE_BOOST_BEAST="${HTTP_BEAST}" \
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
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF_UNIT

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
    build_projects
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
