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
# Tighten vendor udev rules in place (issue #2290). A rule whose MODE lets every local user open the
# device (a last octal digit that is not 0, such as the vendors' MODE="0666") is rewritten to
# GROUP="plugdev", MODE="0660" (keeping each = or := operator): a GROUP already on the line becomes
# plugdev, and a line with none gets one. Comment lines and rules that already keep other users out are left alone.
#
# The vendor files under AlpacaCore/external are regenerated on every SDK bump, so they are never
# edited: debian/rules runs this over the staged rules directory, and scripts/host_setup.sh runs it on
# a copy before installing. scripts/check_docs_drift.py check 31 runs it over every rule debian/rules
# installs and fails if one is still world-accessible.
#
# Usage: scripts/tighten_udev_rules.sh RULES_FILE...
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 RULES_FILE..." >&2
  exit 2
fi

world_mode='MODE[[:space:]]*:?=[[:space:]]*"?0?[0-7]{2}[1-7]"?([^0-7]|$)'
# No `sed -i`: GNU and BSD sed spell it differently, and check_docs_drift.py runs this script on macOS too.
# `cat >` rewrites the file in place, keeping its owner and mode.
tightened="$(mktemp)"
trap 'rm -f "${tightened}"' EXIT
for rules in "$@"; do
  sed -E \
    -e '/^[[:space:]]*#/b' \
    -e "/${world_mode}/!b" \
    -e 's/GROUP([[:space:]]*:?=[[:space:]]*)"[^"]*"/GROUP\1"plugdev"/g' \
    -e '/GROUP[[:space:]]*:?=[[:space:]]*"plugdev"/!s/MODE[[:space:]]*:?=/GROUP="plugdev", &/' \
    -e 's/(MODE[[:space:]]*:?=[[:space:]]*)"0?[0-7]{2}[1-7]"/\1"0660"/g' \
    -e 's/(MODE[[:space:]]*:?=[[:space:]]*)0?[0-7]{2}[1-7]([^0-7]|$)/\1"0660"\2/g' \
    "${rules}" > "${tightened}"
  cat "${tightened}" > "${rules}"
done
