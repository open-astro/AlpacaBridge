#!/usr/bin/env bash
# AlpacaBridge
# Copyright (c) 2025-2026 Joey Troy and contributors
#
# This file is part of AlpacaBridge.
#
# AlpacaBridge is licensed under the GNU Affero General Public License,
# version 3 or (at your option) any later version (AGPL-3.0-or-later),
# with an additional permission allowing combination with proprietary
# device-vendor SDKs. See the LICENSE file in this repository for the full
# license text and the vendor-SDK linking exception, or the license online at:
# https://www.gnu.org/licenses/agpl-3.0.html
#
# Host setup shared by build_and_run.sh and install_alpaca_service.sh (issue #1698): the vendor udev
# rules, the QHY firmware and fxload, the vendor shared libraries, and the dialout and gpio groups.
# Both scripts source this file, so a source build installed either way gets the same host. The .deb
# does the same work in debian/rules and the maintainer scripts. Tested by scripts/test_host_setup.sh.
#
# Sourced, never run. The caller sets `set -euo pipefail`. Every privileged step goes through sudo.

# Copy every vendor udev rule (one copy per basename: the QHY SDK ships three of 85-qhyccd.rules), the
# QHY firmware and its FX3-capable fxload, and the vendor shared libraries companion software loads,
# then reload udev. $1: the AlpacaCore directory.
host_setup_install_vendor_runtime() {
  local core_dir="$1"
  local -A seen_rules=()
  local rules_src=() rule base
  while IFS= read -r -d '' rule; do
    base="$(basename "${rule}")"
    if [[ -z "${seen_rules[${base}]+_}" ]]; then
      seen_rules["${base}"]=1
      rules_src+=("${rule}")
    fi
  done < <(find "${core_dir}/external" -name "*.rules" -type f -print0 | sort -z)

  if [[ ${#rules_src[@]} -eq 0 ]]; then
    echo "No udev rules found under ${core_dir}/external"
  else
    for rule in "${rules_src[@]}"; do
      echo "Installing udev rule: ${rule}"
      sudo install -m 644 "${rule}" /etc/udev/rules.d/
    done
  fi

  # QHY cameras need firmware the udev rules load with fxload. The apt fxload has no -t fx3 (FX3-based
  # cameras); the QHY SDK ships one that does, for the rule's RUN+="/sbin/fxload -t fx3 ...".
  local qhy_sdk_dir="${core_dir}/external/QHY/sdk_linux_arm64"
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

  # Camera and wheel libraries that companion software (SmartGuider) loads by name. Directory, then the
  # library's file name, then what it is.
  local entry dir lib what
  for entry in \
    "ZWO/ASI_Camera_SDK/lib/linux/armv8|libASICamera2.so|ZWO ASI Camera" \
    "SVBONY/lib/armv8|libSVBCameraSDK.so|SVBONY Camera" \
    "ToupTek/toupcamsdk/linux/arm64/glibc|libtoupcam.so|ToupTek Camera" \
    "Altair/altaircamsdk/linux/arm64/glibc|libaltaircam.so|Altair Camera" \
    "PlayerOne/PlayerOne_Camera_SDK_Linux/lib/arm64|libPlayerOneCamera.so|Player One Camera" \
    "PlayerOne/PlayerOne_FilterWheel_SDK_Linux/lib/arm64|libPlayerOnePW.so|Player One Phoenix Filter Wheel"; do
    IFS='|' read -r dir lib what <<< "${entry}"
    if [[ -f "${core_dir}/external/${dir}/${lib}" ]]; then
      echo "Installing ${what} shared library to /usr/local/lib"
      sudo cp -a "${core_dir}/external/${dir}/${lib}"* /usr/local/lib/
      sudo ldconfig
    fi
  done

  sudo udevadm control --reload-rules
  sudo udevadm trigger
}

# A hash of every file host_setup_install_vendor_runtime() installs, so build_and_run.sh re-runs it
# when any of them changes. $1: the AlpacaCore directory.
host_setup_input_hash() {
  local core_dir="$1"
  find "${core_dir}/external" \
    -type f \
    \( -name '*.rules' \
      -o -name 'libASICamera2.so*' \
      -o -name 'libqhyccd.so*' \
      -o -name 'libSVBCameraSDK.so*' \
      -o -name 'libtoupcam.so*' \
      -o -name 'libaltaircam.so*' \
      -o -name 'libPlayerOneCamera.so*' \
      -o -name 'libPlayerOnePW.so*' \
      -o -name 'fxload' \
      -o -path '*/lib/firmware/qhy/*' \) \
    -printf '%s %p\n' 2>/dev/null \
    | sort | sha256sum | awk '{print $1}'
}

# Add the user to dialout (serial mounts, focusers and wheels) unless already a member. $1: ON|OFF.
host_setup_ensure_dialout() {
  [[ "${1:-ON}" == "ON" ]] || return 0
  # The account the service runs as (install_alpaca_service.sh writes User=$(id -un)), not $USER, which may be
  # unset or stale under sudo or automation.
  local user
  user="$(id -un)"
  if id -nG | tr ' ' '\n' | grep -q '^dialout$'; then
    echo "User ${user} is already in the dialout group."
  else
    echo "Adding ${user} to the dialout group (required for serial/USB device access)."
    sudo usermod -aG dialout "${user}"
    echo "You must log out and back in (or run: newgrp dialout) for the change to take effect."
  fi
}

# Make sure the gpio group exists and the user is in it, and give existing GPIO nodes to it. Group
# membership and node ownership are per-system state, not per-SDK-file state, so callers run this on
# every invocation; each step is a no-op when already done. $1: ON|OFF.
host_setup_ensure_gpio() {
  [[ "${1:-ON}" == "ON" ]] || return 0
  [[ "${OSTYPE:-}" == "linux"* ]] || return 0
  local user
  user="$(id -un)"
  # Raspberry Pi OS ships the group; stock Debian and Ubuntu do not. Without it a udev rule's
  # GROUP="gpio" falls back to root and the service gets EACCES at open time.
  if ! getent group gpio >/dev/null; then
    echo "Creating gpio group (required for libgpiod and pwm_gpio kernel-module drivers)."
    sudo groupadd -f gpio
  fi
  if ! id -nG | tr ' ' '\n' | grep -q '^gpio$'; then
    echo "Adding ${user} to the gpio group (required for ZWO ASIair Pro / Plus power switches)."
    sudo usermod -aG gpio "${user}"
    echo "You must log out and back in (or run: newgrp gpio) for the change to take effect."
  fi
  # Nodes created before the group existed: fix them now rather than at the next module reload or
  # hot-plug. Only when not already gpio:0660, so a no-op run asks for no sudo.
  local node current
  for node in /dev/pwm-gpio-misc /dev/gpiochip*; do
    [[ -e "${node}" ]] || continue
    current="$(stat -c '%G %a' "${node}" 2>/dev/null || echo '')"
    if [[ "${current}" != "gpio 660" ]]; then
      sudo chgrp gpio "${node}" 2>/dev/null || true
      sudo chmod 0660 "${node}" 2>/dev/null || true
    fi
  done
}
