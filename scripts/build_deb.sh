#!/usr/bin/env bash
# Build the AlpacaBridge .deb.
#
# debian/changelog is a generated build artifact (gitignored), derived from
# the root CHANGELOG.md so release history is maintained in exactly one
# place. dpkg-buildpackage reads debian/changelog before debian/rules runs,
# so it must be generated here, not inside the rules file.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

PACKAGE="alpacabridge"
MAINTAINER="OpenAstro <support@openastro.net>"
VERSION="$(tr -d '[:space:]' < VERSION)"

if [ -z "${VERSION}" ]; then
    echo "ERROR: VERSION file is empty" >&2
    exit 1
fi

echo "[STEP] Generating debian/changelog from CHANGELOG.md (version ${VERSION})..."
python3 scripts/changelog_to_deb.py \
    --changelog CHANGELOG.md \
    --out debian/changelog \
    --package "${PACKAGE}" \
    --version "${VERSION}" \
    --maintainer "${MAINTAINER}"

dpkg-parsechangelog -l debian/changelog --all >/dev/null
echo "[STEP] debian/changelog generated and parses cleanly."

echo "[STEP] Building .deb with dpkg-buildpackage..."
dpkg-buildpackage -us -uc -b "$@"

echo "[DONE] Package built. See ../${PACKAGE}_${VERSION}_$(dpkg --print-architecture).deb (or ../ for artifacts)."
