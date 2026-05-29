#!/usr/bin/env bash
# Build and run AlpacaBridge.
#
# Default behavior is INCREMENTAL: only files that changed since the last
# build are recompiled. Subsequent runs after an edit take seconds; runs
# with no edits take ~5 seconds (CMake says "nothing to do").
#
# Optional env:
#   CLEAN=1                          Force a full from-scratch rebuild
#                                    (removes AlpacaHTTP/build/ first).
#                                    Use after toolchain upgrades or when
#                                    things look genuinely wrong.
#   ALPACA_INSTALL_UDEV_RULES=ON|OFF  default ON
#   ALPACA_ADD_DIALOUT=ON|OFF         default ON
#   ALPACA_INSTALL_ACCELERATORS=ON|OFF default ON. When ON, auto-installs
#                                    ccache + ninja-build via apt the first
#                                    time they are missing (uses sudo).
#   ALPACACORE_ENABLE_ALL_VENDORS    default ON
#   ALPACAHTTP_USE_BOOST_BEAST       default OFF
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
HTTP_BEAST="${ALPACAHTTP_USE_BOOST_BEAST:-OFF}"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
INSTALL_UDEV_RULES="${ALPACA_INSTALL_UDEV_RULES:-ON}"
INSTALL_ACCELERATORS="${ALPACA_INSTALL_ACCELERATORS:-ON}"
ADD_DIALOUT="${ALPACA_ADD_DIALOUT:-ON}"
CLEAN="${CLEAN:-0}"

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi
if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

# ---------------------------------------------------------------------------
# Build-accelerator install: ccache + ninja-build
# ---------------------------------------------------------------------------
# Both are tiny, stable Debian packages with no surprises. We check on every
# run; the check is a microsecond `command -v`. If a package is missing AND
# we have sudo AND apt, we install it. The user sees one apt install on a
# fresh Pi; every subsequent run is a no-op detection.
if [[ "${INSTALL_ACCELERATORS}" == "ON" && "${OSTYPE:-}" == "linux"* ]]; then
  needed_pkgs=()
  command -v ccache >/dev/null 2>&1 || needed_pkgs+=(ccache)
  command -v ninja  >/dev/null 2>&1 || needed_pkgs+=(ninja-build)
  if (( ${#needed_pkgs[@]} > 0 )); then
    if command -v sudo >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1; then
      echo "Installing build accelerators: ${needed_pkgs[*]}"
      sudo apt-get update -qq
      sudo apt-get install -y "${needed_pkgs[@]}"
    else
      echo "Note: missing accelerators (${needed_pkgs[*]}) and no sudo+apt available; falling back to Make + no compiler cache."
    fi
  fi
fi

CMAKE_GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  CMAKE_GENERATOR_ARGS+=(-G Ninja)
fi
COMPILER_LAUNCHER_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  COMPILER_LAUNCHER_ARGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# ---------------------------------------------------------------------------
# CLEAN=1 forces a full rebuild. Otherwise we keep AlpacaHTTP/build/.
# ---------------------------------------------------------------------------
# Note: only AlpacaHTTP/build/ matters at runtime. AlpacaHTTP pulls
# AlpacaCore via add_subdirectory(../AlpacaCore), so the AlpacaCore/build/
# directory the old script produced was a redundant standalone artifact
# (libalpacacore.a) never used by the server. We don't create it any more.
if [[ "${CLEAN}" == "1" ]]; then
  echo "CLEAN=1 -- removing ${HTTP_DIR}/build for full rebuild"
  rm -rf "${HTTP_DIR}/build"
fi

# Also auto-clean if the cached generator doesn't match what we want to use
# this run (i.e. user previously ran the old script with Make, but Ninja is
# now installed). CMake refuses to reconfigure across generators, so without
# this check the build would fail with a confusing error.
if [[ -f "${HTTP_DIR}/build/CMakeCache.txt" ]]; then
  existing_generator="$(grep '^CMAKE_GENERATOR:' "${HTTP_DIR}/build/CMakeCache.txt" 2>/dev/null | head -1 | cut -d= -f2)"
  desired_generator="Unix Makefiles"
  if command -v ninja >/dev/null 2>&1; then
    desired_generator="Ninja"
  fi
  if [[ -n "${existing_generator}" && "${existing_generator}" != "${desired_generator}" ]]; then
    echo "Build dir was configured with '${existing_generator}' but we want '${desired_generator}'; cleaning ${HTTP_DIR}/build"
    rm -rf "${HTTP_DIR}/build"
  fi
fi

# ---------------------------------------------------------------------------
# /var/log/AlpacaBridge — best effort, idempotent
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# udev rules / vendor firmware / vendor .so installation — cached via stamp.
# ---------------------------------------------------------------------------
# We hash everything we'd install, write the hash to .build_setup_stamp after
# a successful install, and skip the entire block on subsequent runs when
# nothing has changed. Delete the stamp (or pass CLEAN=1) to force a re-run.
SETUP_STAMP="${HTTP_DIR}/build/.system_setup_stamp"

run_system_setup() {
  arch="$(uname -m)"
  if [[ "${arch}" != "aarch64" && "${arch}" != "arm64" ]]; then
    echo "AlpacaBridge supports Linux arm64 only. Detected: ${arch}"
    exit 1
  fi

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

  qhy_sdk_dir="${CORE_DIR}/external/QHY/sdk_Arm64_25.09.29"
  if [[ -d "${qhy_sdk_dir}/lib/firmware/qhy" ]]; then
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

  zwo_camera_lib_dir="${CORE_DIR}/external/ZWO/ASI_Camera_SDK/lib/linux/armv8"
  if [[ -f "${zwo_camera_lib_dir}/libASICamera2.so" ]]; then
    echo "Installing ZWO ASI Camera shared library to /usr/local/lib"
    sudo cp -a "${zwo_camera_lib_dir}/libASICamera2.so"* /usr/local/lib/
    sudo ldconfig
  fi

  svbony_lib_dir="${CORE_DIR}/external/SVBONY/lib/armv8"
  if [[ -f "${svbony_lib_dir}/libSVBCameraSDK.so" ]]; then
    echo "Installing SVBONY Camera shared library to /usr/local/lib"
    sudo cp -a "${svbony_lib_dir}/libSVBCameraSDK.so"* /usr/local/lib/
    sudo ldconfig
  fi

  touptek_lib_dir="${CORE_DIR}/external/ToupTek/toupcamsdk.20260128/linux/arm64/glibc"
  if [[ -f "${touptek_lib_dir}/libtoupcam.so" ]]; then
    echo "Installing ToupTek Camera shared library to /usr/local/lib"
    sudo cp -a "${touptek_lib_dir}/libtoupcam.so"* /usr/local/lib/
    sudo ldconfig
  fi

  playerone_lib_dir="${CORE_DIR}/external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0/lib/arm64"
  if [[ -f "${playerone_lib_dir}/libPlayerOneCamera.so" ]]; then
    echo "Installing Player One Camera shared library to /usr/local/lib"
    sudo cp -a "${playerone_lib_dir}/libPlayerOneCamera.so"* /usr/local/lib/
    sudo ldconfig
  fi

  sudo udevadm control --reload-rules
  sudo udevadm trigger

  if [[ "${ADD_DIALOUT}" == "ON" ]]; then
    if id -nG | tr ' ' '\n' | grep -q '^dialout$'; then
      echo "User ${USER} is already in the dialout group."
    else
      echo "Adding ${USER} to the dialout group (required for serial/USB device access)."
      sudo usermod -aG dialout "${USER}"
      echo "You must log out and back in (or run: newgrp dialout) for the change to take effect."
    fi
  fi
}

if [[ "${INSTALL_UDEV_RULES}" == "ON" && "${OSTYPE:-}" == "linux"* ]]; then
  # Hash every input the setup block would consume, so any change (vendor SDK
  # update, new rules file, new firmware blob, new udev rule, etc.) re-runs
  # the install automatically.
  setup_hash=$(find "${CORE_DIR}/external" \
                 -type f \
                 \( -name '*.rules' \
                   -o -name 'libASICamera2.so*' \
                   -o -name 'libqhyccd.so*' \
                   -o -name 'libSVBCameraSDK.so*' \
                   -o -name 'libtoupcam.so*' \
                   -o -name 'libPlayerOneCamera.so*' \
                   -o -name 'fxload' \) \
                 -printf '%s %p\n' 2>/dev/null \
                 | sort | sha256sum | awk '{print $1}')

  if [[ -f "${SETUP_STAMP}" ]] && [[ "$(cat "${SETUP_STAMP}" 2>/dev/null)" == "${setup_hash}" ]]; then
    # Nothing changed since last install — skip the sudo prompt block entirely.
    :
  else
    run_system_setup
    mkdir -p "$(dirname "${SETUP_STAMP}")"
    echo "${setup_hash}" > "${SETUP_STAMP}"
  fi
fi

# ---------------------------------------------------------------------------
# Build (incremental by default)
# ---------------------------------------------------------------------------
if [[ "${OSTYPE:-}" == "darwin"* ]]; then
  PARALLEL="$(sysctl -n hw.ncpu)"
elif command -v nproc >/dev/null 2>&1; then
  PARALLEL="$(nproc)"
else
  PARALLEL="4"
fi

# Build AlpacaHTTP only -- it pulls AlpacaCore as a subdirectory, so we don't
# need to build AlpacaCore standalone. CMake's reconfigure step is cheap when
# nothing changed; the build step is incremental and skips up-to-date files.
echo "== AlpacaHTTP =="
cmake -S "${HTTP_DIR}" -B "${HTTP_DIR}/build" \
  "${CMAKE_GENERATOR_ARGS[@]}" \
  "${COMPILER_LAUNCHER_ARGS[@]}" \
  -DALPACAHTTP_USE_BOOST_BEAST="${HTTP_BEAST}" \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}" \
  -DALPACAHTTP_BUILD_TESTS=OFF \
  -DALPACACORE_BUILD_TESTS=OFF
cmake --build "${HTTP_DIR}/build" --parallel "${PARALLEL}"

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
for candidate in \
  "${HTTP_DIR}/build/alpacahttp_server" \
  "${HTTP_DIR}/build/Debug/alpacahttp_server" \
  "${HTTP_DIR}/build/Release/alpacahttp_server"; do
  if [[ -x "${candidate}" ]]; then
    echo "AlpacaHTTP is running. Open http://localhost:6800/ in your browser."
    exec "${candidate}"
  fi
done

echo "Could not find alpacahttp_server binary in ${HTTP_DIR}/build"
exit 1
