#!/usr/bin/env bash
# Optional env: ALPACA_INSTALL_UDEV_RULES=ON|OFF, ALPACA_ADD_DIALOUT=ON|OFF, ALPACACORE_ENABLE_ALL_VENDORS, ALPACAHTTP_USE_BOOST_BEAST
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
HTTP_BEAST="${ALPACAHTTP_USE_BOOST_BEAST:-OFF}"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
INSTALL_UDEV_RULES="${ALPACA_INSTALL_UDEV_RULES:-ON}"
ADD_DIALOUT="${ALPACA_ADD_DIALOUT:-ON}"

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi

if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

rm -rf "${CORE_DIR}/build" "${HTTP_DIR}/build"

# Try to create /var/log/AlpacaBridge so logs land in the standard path. Best
# effort only: if sudo isn't available, the file-logging sink will fall back to
# ~/.local/state/AlpacaBridge/logs on first run, so don't abort the build.
if [[ "${OSTYPE:-}" == "linux"* ]]; then
  LOG_DIR="/var/log/AlpacaBridge"
  TARGET_USER="${SUDO_USER:-${USER:-$(id -un)}}"
  TARGET_GROUP="$(id -gn "${TARGET_USER}" 2>/dev/null || echo "${TARGET_USER}")"
  if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    if [[ ! -d "${LOG_DIR}" ]]; then
      echo "Creating ${LOG_DIR} for AlpacaBridge logs..."
      sudo mkdir -p "${LOG_DIR}"
    fi
    sudo chown "${TARGET_USER}:${TARGET_GROUP}" "${LOG_DIR}"
    sudo chmod 0755 "${LOG_DIR}"
  else
    echo "Note: skipping ${LOG_DIR} setup (sudo unavailable). The server will use its user-state fallback log directory."
  fi
fi

if [[ "${INSTALL_UDEV_RULES}" == "ON" && "${OSTYPE:-}" == "linux"* ]]; then
  declare -A seen_rules
  RULES_SRC=()
  while IFS= read -r -d '' rule; do
    base="$(basename "${rule}")"
    if [[ -z "${seen_rules[${base}]+_}" ]]; then
      seen_rules["${base}"]=1
      RULES_SRC+=("${rule}")
    fi
  done < <(find "${CORE_DIR}/external" -name "*.rules" -type f -print0 | sort -z)
  for rule in "${RULES_SRC[@]}"; do
    echo "Installing udev rule: ${rule}"
    sudo install -m 644 "${rule}" /etc/udev/rules.d/
  done

  arch="$(uname -m)"
  qhy_sdk_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    qhy_sdk_dir="${CORE_DIR}/external/QHY/sdk_Arm64_25.09.29"
  elif [[ "${arch}" == "x86_64" ]]; then
    qhy_sdk_dir="${CORE_DIR}/external/QHY/sdk_linux64_25.09.29"
  fi
  if [[ -n "${qhy_sdk_dir}" && -d "${qhy_sdk_dir}/lib/firmware/qhy" ]]; then
    echo "Installing QHY firmware files from ${qhy_sdk_dir}/lib/firmware/qhy"
    sudo mkdir -p /lib/firmware/qhy
    sudo cp -a "${qhy_sdk_dir}/lib/firmware/qhy/." /lib/firmware/qhy/
    if [[ -f "${qhy_sdk_dir}/sbin/fxload" ]]; then
      echo "Installing QHY SDK fxload (FX3-capable) to /sbin/fxload"
      sudo install -m 755 "${qhy_sdk_dir}/sbin/fxload" /sbin/fxload
    fi
    if [[ -d "${qhy_sdk_dir}/usr/local/lib" ]]; then
      echo "Installing QHY shared libraries to /usr/local/lib"
      sudo cp -a "${qhy_sdk_dir}/usr/local/lib/libqhyccd.so"* /usr/local/lib/
      sudo ldconfig
    fi
  fi

  # Install ZWO ASI Camera shared library (used by SmartGuider via zwoasi)
  zwo_camera_lib_dir=""
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
  svbony_lib_dir=""
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
  touptek_lib_dir=""
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

  # Install Player One Camera shared library (SmartGuider dlopens this directly)
  playerone_lib_dir=""
  if [[ "${arch}" == "aarch64" || "${arch}" == "arm64" ]]; then
    playerone_lib_dir="${CORE_DIR}/external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0/lib/arm64"
  elif [[ "${arch}" == "x86_64" ]]; then
    playerone_lib_dir="${CORE_DIR}/external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0/lib/x64"
  fi
  if [[ -n "${playerone_lib_dir}" && -f "${playerone_lib_dir}/libPlayerOneCamera.so" ]]; then
    echo "Installing Player One Camera shared library to /usr/local/lib"
    sudo cp -a "${playerone_lib_dir}/libPlayerOneCamera.so"* /usr/local/lib/
    sudo ldconfig
  fi

  sudo udevadm control --reload-rules
  sudo udevadm trigger

  if [[ "${ADD_DIALOUT}" == "ON" ]]; then
    if id -nG | tr ' ' '\n' | grep -q '^dialout$'; then
      echo "User $USER is already in the dialout group."
    else
      echo "Adding $USER to the dialout group (required for serial/USB device access)."
      sudo usermod -aG dialout "$USER"
      echo "You must log out and back in (or run: newgrp dialout) for the change to take effect."
    fi
  fi
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
      -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}" \
      -DALPACAHTTP_BUILD_TESTS=OFF
  else
    cmake -S "${project_dir}" -B "${project_dir}/build" \
      -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}" \
      -DALPACACORE_BUILD_TESTS=OFF
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
