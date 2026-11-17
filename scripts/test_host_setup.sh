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
# Self-test for the host setup that build_and_run.sh and install_alpaca_service.sh share through
# scripts/host_setup.sh (issue #1698). It runs `install_alpaca_service.sh install` with shims on PATH:
# sudo records each privileged command and runs nothing, cmake and systemctl do nothing, and id,
# getent and uname answer as a fresh arm64 Debian host whose user is in neither group. It then checks
# which privileged commands the install asked for. Nothing on the host is changed. Linux only (the
# install script refuses other systems); elsewhere it prints SKIP and passes.
#
# Usage: scripts/test_host_setup.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "${OSTYPE:-}" != linux* ]]; then
  echo "SKIP: the host setup self-test runs on Linux only"
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
shims="${tmp}/bin"
mkdir -p "${shims}"
log="${tmp}/sudo.log"

installed_rules="${tmp}/installed-rules"
# sudo: record, run nothing. `sudo tee` reads the unit file from stdin, so drain it. A udev rule the
# setup installs (`install -m 644 <rule> /etc/udev/rules.d/`) is copied aside so its text can be read.
cat > "${shims}/sudo" <<EOF
#!/usr/bin/env bash
echo "\$*" >> "${log}"
if [[ "\${1:-}" == tee ]]; then cat > /dev/null; fi
if [[ "\${1:-} \${2:-} \${3:-}" == "install -m 644" && "\${5:-}" == /etc/udev/rules.d/ ]]; then
  mkdir -p "${installed_rules}" && cp "\${4}" "${installed_rules}/"
fi
exit 0
EOF
cat > "${shims}/cmake" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
# systemctl: nothing is active or enabled; every other verb succeeds.
cat > "${shims}/systemctl" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in is-active|is-enabled) exit 1 ;; esac
exit 0
EOF
cat > "${shims}/uname" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == -m ]]; then echo aarch64; exit 0; fi
exec /usr/bin/uname "$@"
EOF
# id: the user and its groups come from SHIM_GROUPS (space separated, the user's own group first).
cat > "${shims}/id" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  -nG) echo "${SHIM_GROUPS}" ;;
  -un|-gn) echo "testuser" ;;
  *) exec /usr/bin/id "$@" ;;
esac
EOF
# getent: the gpio group exists only when SHIM_HAS_GPIO=1, plugdev only when SHIM_HAS_PLUGDEV=1.
cat > "${shims}/getent" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-} ${2:-}" == "group gpio" ]]; then
  [[ "${SHIM_HAS_GPIO:-0}" == 1 ]] && { echo "gpio:x:997:"; exit 0; }
  exit 2
fi
if [[ "${1:-} ${2:-}" == "group plugdev" ]]; then
  [[ "${SHIM_HAS_PLUGDEV:-0}" == 1 ]] && { echo "plugdev:x:46:"; exit 0; }
  exit 2
fi
exec /usr/bin/getent "$@"
EOF
chmod +x "${shims}"/*

failures=0
check() {
  local name="$1" expect="$2" pattern="$3" found=no
  if grep -qE -- "${pattern}" "${log}"; then
    found=yes
  fi
  if [[ "${found}" == "${expect}" ]]; then
    echo "[PASS] ${name}"
  else
    echo "[FAIL] ${name}: expected ${expect}, found ${found} for /${pattern}/"
    failures=$((failures + 1))
  fi
}

# $1: install or update; the rest are env assignments. USER is left unset: the groups go to `id -un`.
run_install() {
  local verb="$1"
  shift
  : > "${log}"
  rm -rf "${installed_rules}"
  env -u USER PATH="${shims}:${PATH}" ALPACACORE_ENABLE_ALL_VENDORS=OFF "$@" \
    bash "${ROOT}/install_alpaca_service.sh" "${verb}" > "${tmp}/out.log" 2>&1 || {
    echo "[FAIL] install_alpaca_service.sh ${verb} exited non-zero:"
    sed 's/^/    /' "${tmp}/out.log"
    failures=$((failures + 1))
  }
}

# A fresh host: no gpio or plugdev group, the user in no group. The install must create the groups and
# add the user to dialout (serial mounts), plugdev (USB devices under the tightened udev rules, issue
# #2290) and gpio (ASIAIR power ports), as build_and_run.sh does.
run_install install SHIM_GROUPS="testuser" SHIM_HAS_GPIO=0 SHIM_HAS_PLUGDEV=0
check "fresh host: udev rules installed" yes '^install -m 644 .*\.rules /etc/udev/rules\.d/'
check "fresh host: gpio group created" yes '^groupadd -f gpio$'
check "fresh host: plugdev group created" yes '^groupadd -f plugdev$'
check "fresh host: user added to dialout" yes '^usermod -aG dialout testuser$'
check "fresh host: user added to plugdev" yes '^usermod -aG plugdev testuser$'
check "fresh host: user added to gpio" yes '^usermod -aG gpio testuser$'

# Issue #2290: every rule the setup installs keeps other users out, and the ones it tightened give the
# device to plugdev. The vendor files in the tree still carry MODE="0666"; only the copies change.
world_rules="$(grep -lE 'MODE[[:space:]]*:?=[[:space:]]*"?0?[0-7]{2}[1-7]([^0-7]|$)' \
  "${installed_rules}"/*.rules 2>/dev/null || true)"
if [[ ! -d "${installed_rules}" ]] || ! ls "${installed_rules}"/*.rules > /dev/null 2>&1; then
  echo "[FAIL] udev rules: the setup installed no rule to read"
  failures=$((failures + 1))
elif [[ -n "${world_rules}" ]]; then
  echo "[FAIL] udev rules: installed rules still let every user open a device: ${world_rules}"
  failures=$((failures + 1))
else
  echo "[PASS] udev rules: no installed rule lets every user open a device"
fi
if grep -q 'GROUP="plugdev", MODE="0660"' "${installed_rules}/asi.rules" 2>/dev/null \
    && grep -q 'MODE="0666"' "${ROOT}/AlpacaCore/external/ZWO/ASI_Camera_SDK/lib/linux/asi.rules"; then
  echo "[PASS] udev rules: the ZWO camera rule is installed as plugdev 0660, the vendor file is unchanged"
else
  echo "[FAIL] udev rules: the installed ZWO camera rule is not plugdev 0660, or the vendor file was edited"
  failures=$((failures + 1))
fi

# Already set up: no group change is asked for.
run_install install SHIM_GROUPS="testuser dialout plugdev gpio" SHIM_HAS_GPIO=1 SHIM_HAS_PLUGDEV=1
check "set-up host: no groupadd" no '^groupadd'
check "set-up host: no usermod" no '^usermod'

# Both opt-outs honoured, as in build_and_run.sh.
run_install install SHIM_GROUPS="testuser" SHIM_HAS_GPIO=0 SHIM_HAS_PLUGDEV=0 ALPACA_ADD_DIALOUT=OFF \
  ALPACA_ADD_PLUGDEV=OFF ALPACA_ADD_GPIO=OFF
check "opt-out: no groupadd" no '^groupadd'
check "opt-out: no usermod" no '^usermod'

# update sets the groups up too (a host installed before the fix gets them on its next update).
run_install update SHIM_GROUPS="testuser" SHIM_HAS_GPIO=0 SHIM_HAS_PLUGDEV=1
check "update: gpio group created" yes '^groupadd -f gpio$'
check "update: an existing plugdev group is not created again" no '^groupadd -f plugdev$'
check "update: user added to dialout" yes '^usermod -aG dialout testuser$'
check "update: user added to plugdev" yes '^usermod -aG plugdev testuser$'
check "update: user added to gpio" yes '^usermod -aG gpio testuser$'

# The setup stamp of build_and_run.sh covers every file the setup installs: a changed Altair or
# Phoenix library, or QHY firmware, must change it so the next run installs it.
# shellcheck source=scripts/host_setup.sh
source "${ROOT}/scripts/host_setup.sh"
fake_core="${tmp}/core"
mkdir -p "${fake_core}/external/Altair" "${fake_core}/external/PlayerOne" "${fake_core}/external/QHY/lib/firmware/qhy"
echo a > "${fake_core}/external/Altair/libaltaircam.so"
echo b > "${fake_core}/external/PlayerOne/libPlayerOnePW.so"
echo c > "${fake_core}/external/QHY/lib/firmware/qhy/QHY5III.img"
base_hash="$(host_setup_input_hash "${fake_core}")"
# The tighten script is an input too: a change to it must re-run the rules install (#2290). The test
# edits a copy (HOST_SETUP_DIR pointed at it), so an interrupted run never leaves the checkout dirty.
setup_copy="${tmp}/setup-copy"
mkdir -p "${setup_copy}"
cp "${ROOT}/scripts/tighten_udev_rules.sh" "${setup_copy}/"
real_setup_dir="${HOST_SETUP_DIR}"
HOST_SETUP_DIR="${setup_copy}"
copy_hash="$(host_setup_input_hash "${fake_core}")"
echo '# changed' >> "${setup_copy}/tighten_udev_rules.sh"
if [[ "$(host_setup_input_hash "${fake_core}")" == "${copy_hash}" ]]; then
  echo "[FAIL] setup stamp: a change to scripts/tighten_udev_rules.sh does not change the hash"
  failures=$((failures + 1))
else
  echo "[PASS] setup stamp covers scripts/tighten_udev_rules.sh"
fi

# A tighten script that fails leaves no staged copy behind, and the rules install reports the failure.
printf '#!/usr/bin/env bash\nexit 1\n' > "${setup_copy}/tighten_udev_rules.sh"
fail_core="${tmp}/core-fail"  # its own tree, so the stamp cases below still compare against base_hash
mkdir -p "${fail_core}/external/V" "${tmp}/tmpdir"
echo 'SUBSYSTEM=="usb", MODE="0666"' > "${fail_core}/external/V/v.rules"
if (TMPDIR="${tmp}/tmpdir" PATH="${shims}:${PATH}" host_setup_install_vendor_runtime "${fail_core}") \
    > /dev/null 2>&1; then
  echo "[FAIL] udev install: a failing tighten script did not fail the install"
  failures=$((failures + 1))
elif [[ -n "$(ls -A "${tmp}/tmpdir")" ]]; then
  echo "[FAIL] udev install: a failed tighten left its staged copies in ${tmp}/tmpdir"
  failures=$((failures + 1))
else
  echo "[PASS] udev install: a failed tighten fails the install and removes its staged copies"
fi
HOST_SETUP_DIR="${real_setup_dir}"
for f in Altair/libaltaircam.so PlayerOne/libPlayerOnePW.so QHY/lib/firmware/qhy/QHY5III.img; do
  cp "${fake_core}/external/${f}" "${tmp}/saved"
  echo changed-and-longer >> "${fake_core}/external/${f}"
  if [[ "$(host_setup_input_hash "${fake_core}")" == "${base_hash}" ]]; then
    echo "[FAIL] setup stamp: a change to ${f} does not change the hash"
    failures=$((failures + 1))
  else
    echo "[PASS] setup stamp covers ${f}"
  fi
  cp "${tmp}/saved" "${fake_core}/external/${f}"
done

if [[ "${failures}" -ne 0 ]]; then
  echo "host setup self-test: ${failures} failure(s)"
  exit 1
fi
echo "host setup self-test: OK"
