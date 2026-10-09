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

# A beta (X.Y.Z~betaN) has no dated CHANGELOG section; its stanza is dated
# from the README badge (the day the beta was cut) so the build is reproducible.
DATE_ARGS=()
# changelog_fragments.py owns the beta spelling (0 = beta, 1 = bare X.Y.Z,
# 2 = anything else, such as an ~rc1 test build, which needs no date).
beta_rc=0
python3 scripts/changelog_fragments.py --is-beta "${VERSION}" 2>/dev/null || beta_rc=$?
if [ "${beta_rc}" -eq 0 ]; then
  # The same parser docs-drift check 16 uses, so the two cannot drift apart.
  BADGE_DATE="$(python3 scripts/check_docs_drift.py --badge-date)"
  DATE_ARGS=(--date "${BADGE_DATE}")
fi

echo "[STEP] Generating debian/changelog from CHANGELOG.md (version ${VERSION})..."
python3 scripts/changelog_to_deb.py \
    --changelog CHANGELOG.md \
    --out debian/changelog \
    --package "${PACKAGE}" \
    --version "${VERSION}" \
    --maintainer "${MAINTAINER}" \
    "${DATE_ARGS[@]}"

dpkg-parsechangelog -l debian/changelog --all >/dev/null
echo "[STEP] debian/changelog generated and parses cleanly."

echo "[STEP] Building .deb with dpkg-buildpackage..."
dpkg-buildpackage -us -uc -b "$@"

echo "[DONE] Package built. See ../${PACKAGE}_${VERSION}_$(dpkg --print-architecture).deb (or ../ for artifacts)."
