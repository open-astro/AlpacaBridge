#!/usr/bin/env bash
# Pin the VERSION split that AlpacaCore/CMakeLists.txt and
# AlpacaHTTP/CMakeLists.txt share through AlpacaCore/cmake/AlpacaBridgeVersion.cmake
# (docs/beta-channel.md): project() takes the numeric part of a beta VERSION
# (5.0.0~beta1 -> 5.0.0) and the ALPACACORE_VERSION / ALPACAHTTP_VERSION defines
# keep the full string. The helper runs in script mode (cmake -P) over sample
# values, so nothing is configured or compiled; the ordinary configure in CI
# and the pre-flight exercises the include itself. CI runs this in build-test,
# the pre-flight in gate 3b.
#
# Usage: scripts/check_beta_configure.sh [X.Y.Z~betaN]   (default 5.0.0~beta1)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HELPER="${ROOT_DIR}/AlpacaCore/cmake/AlpacaBridgeVersion.cmake"
BETA="${1:-5.0.0~beta1}"
# changelog_fragments.py owns the spelling: 0 means X.Y.Z~betaN.
if ! python3 "${ROOT_DIR}/scripts/changelog_fragments.py" --is-beta "${BETA}" 2>/dev/null; then
  echo "ERROR: '${BETA}' is not X.Y.Z~betaN" >&2
  exit 2
fi
BASE="${BETA%%~*}"

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
cat > "${tmp}/split.cmake" <<CMAKE
include("${HELPER}")
alpacabridge_read_version("\${VERSION_FILE}")
message(STATUS "\${ALPACABRIDGE_VERSION}|\${ALPACABRIDGE_BASE_VERSION}")
CMAKE

fail=0
split() {  # <VERSION text> -> "full|base", or nonzero when the helper refuses it
  printf '%s\n' "$1" > "${tmp}/VERSION"
  cmake -DVERSION_FILE="${tmp}/VERSION" -P "${tmp}/split.cmake" 2>&1 | sed -n 's/^-- //p'
}
for pair in "${BETA}|${BETA}|${BASE}" "4.2.0|4.2.0|4.2.0" "  ${BETA}  |${BETA}|${BASE}"; do
  input="${pair%%|*}"; want="${pair#*|}"
  got="$(split "${input}" || true)"
  if [ "${got}" != "${want}" ]; then
    echo "FAIL: VERSION '${input}' split as '${got:-nothing}', expected '${want}' (full|base)" >&2
    fail=1
  fi
done
printf '5.0~beta1\n' > "${tmp}/VERSION"
if cmake -DVERSION_FILE="${tmp}/VERSION" -P "${tmp}/split.cmake" >/dev/null 2>&1; then
  echo "FAIL: VERSION '5.0~beta1' was accepted; project() would reject it with a less useful error" >&2
  fail=1
fi

# Both CMakeLists use the helper, give project() the base and the defines the full string.
for spec in "AlpacaCore/CMakeLists.txt|ALPACACORE_VERSION" "AlpacaHTTP/CMakeLists.txt|ALPACAHTTP_VERSION"; do
  file="${ROOT_DIR}/${spec%%|*}"; define="${spec#*|}"
  grep -q 'alpacabridge_read_version(' "${file}" \
    || { echo "FAIL: ${spec%%|*} does not call alpacabridge_read_version()" >&2; fail=1; }
  grep -Eq '^project\([A-Za-z]+ VERSION \$\{ALPACABRIDGE_BASE_VERSION\}' "${file}" \
    || { echo "FAIL: ${spec%%|*}: project() does not take \${ALPACABRIDGE_BASE_VERSION}" >&2; fail=1; }
  grep -Eq "${define}=\\\\?\"\\\$\\{ALPACABRIDGE_VERSION\\}\\\\?\"" "${file}" \
    || { echo "FAIL: ${spec%%|*}: ${define} is not \${ALPACABRIDGE_VERSION}" >&2; fail=1; }
done
if [ "${fail}" -ne 0 ]; then
  exit 1
fi
echo "Beta VERSION split OK (${BETA}: project() ${BASE}, both defines keep ${BETA})."
