#!/usr/bin/env bash
#
# Local CI pre-flight for AlpacaBridge.
#
# Reproduces the PR gates in .github/workflows/ci.yml on this (arm64) machine so
# a branch can be proven clean *before* it is pushed and a PR is opened. The
# /submit-pr skill calls this; it is also runnable by hand:
#
#   ./scripts/ci_preflight.sh                 # base = main
#   PREFLIGHT_BASE=upstream/main ./scripts/ci_preflight.sh   # fork contributors
#   RUN_SANITIZERS=1 ./scripts/ci_preflight.sh # also run the ASan+UBSan job
#   PREFLIGHT_NO_INSTALL=1 ./scripts/ci_preflight.sh         # never apt-install
#
# Missing analysis tools (clang-tidy, cppcheck, shellcheck, clang-format, node)
# are auto-installed via `sudo apt-get` unless PREFLIGHT_NO_INSTALL=1, in which case
# the corresponding check is reported SKIP and the run still fails loudly if a
# mandatory tool could not be obtained. CI runs these regardless, so a skip is
# never silently treated as a pass. zizmor is not in apt, so it is fetched as a
# pinned, checksum-verified release binary into a user cache (no sudo); keep
# ZIZMOR_VER / ZIZMOR_SHA256 below in sync with .github/workflows/ci.yml.
#
# Exit status: 0 only if every mandatory check passed.

# Intentionally NOT using `set -e`: we want to run every gate and report a full
# summary rather than abort on the first failure. -u and pipefail still apply.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}" || exit 1

BASE="${PREFLIGHT_BASE:-main}"

if command -v nproc >/dev/null 2>&1; then
  PARALLEL="$(nproc)"
else
  PARALLEL="4"
fi

# --- result tracking -------------------------------------------------------

OVERALL=0
declare -a RESULTS=()

record() { # <status: PASS|FAIL|SKIP> <name>
  RESULTS+=("$1|$2")
  if [ "$1" = "FAIL" ]; then
    OVERALL=1
  fi
}

section() { printf '\n========== %s ==========\n' "$1"; }

# --- tool auto-install -----------------------------------------------------

APT_UPDATED=0

apt_update_once() {
  if [ "${APT_UPDATED}" != "1" ]; then
    sudo apt-get update -qq || true
    APT_UPDATED=1
  fi
}

# ensure_tool <command> <apt-package>: returns 0 if the command is available
# (installing it first when missing and allowed), non-zero otherwise.
ensure_tool() {
  local cmd="$1" pkg="$2"
  if command -v "${cmd}" >/dev/null 2>&1; then
    return 0
  fi
  if [ "${PREFLIGHT_NO_INSTALL:-0}" = "1" ]; then
    echo ">> ${cmd} missing and PREFLIGHT_NO_INSTALL=1 -- not installing."
    return 1
  fi
  echo ">> Installing ${pkg} (provides ${cmd})..."
  apt_update_once
  sudo apt-get install -y --no-install-recommends "${pkg}" >/dev/null 2>&1
  command -v "${cmd}" >/dev/null 2>&1
}

# Pinned zizmor release (keep in sync with .github/workflows/ci.yml). GitHub
# release assets are immutable, so the hash is stable for a given version.
ZIZMOR_VER="1.25.2"
ZIZMOR_SHA256="4b4b9491112c2a09b318101c0d3349b73af1c4f532e097dd6d0164f2abda760d"

# ensure_zizmor: prints a usable zizmor binary path on stdout and returns 0, or
# returns non-zero if unavailable. zizmor is not packaged in apt, so (mirroring
# CI) we download the pinned aarch64 release, verify its checksum, and cache the
# binary under the user cache dir -- no sudo, no system mutation. Progress goes
# to stderr so stdout carries only the path.
ensure_zizmor() {
  if command -v zizmor >/dev/null 2>&1; then
    command -v zizmor
    return 0
  fi
  local cache_dir bin tgz url
  cache_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/alpacabridge-preflight"
  bin="${cache_dir}/zizmor-${ZIZMOR_VER}"
  # A previously cached binary needs no install, so honor it even under
  # PREFLIGHT_NO_INSTALL; that flag only blocks a fresh download.
  if [ -x "${bin}" ]; then
    echo "${bin}"
    return 0
  fi
  if [ "${PREFLIGHT_NO_INSTALL:-0}" = "1" ]; then
    return 1
  fi
  mkdir -p "${cache_dir}"
  tgz="${cache_dir}/zizmor-${ZIZMOR_VER}.tar.gz"
  url="https://github.com/zizmorcore/zizmor/releases/download/v${ZIZMOR_VER}/zizmor-aarch64-unknown-linux-gnu.tar.gz"
  echo ">> Downloading zizmor ${ZIZMOR_VER}..." >&2
  if ! curl -fsSL "${url}" -o "${tgz}"; then
    echo ">> zizmor download failed." >&2
    return 1
  fi
  if ! echo "${ZIZMOR_SHA256}  ${tgz}" | sha256sum --check --strict - >/dev/null 2>&1; then
    echo ">> zizmor checksum mismatch -- refusing to use the download." >&2
    rm -f "${tgz}"
    return 1
  fi
  # Newer archives ship the bare `zizmor` binary at the root; fall back to a
  # full extract + search if the layout ever changes.
  if ! tar -xzf "${tgz}" -C "${cache_dir}" zizmor 2>/dev/null; then
    tar -xzf "${tgz}" -C "${cache_dir}" || { echo ">> zizmor extract failed." >&2; return 1; }
  fi
  if [ ! -f "${cache_dir}/zizmor" ]; then
    echo ">> zizmor binary not found after extraction (archive layout changed?)." >&2
    return 1
  fi
  mv "${cache_dir}/zizmor" "${bin}"
  chmod +x "${bin}"
  rm -f "${tgz}"
  echo "${bin}"
}

# --- changed-file sets -----------------------------------------------------

git fetch --no-tags origin "${BASE#origin/}" >/dev/null 2>&1 || true
MERGE_BASE="$(git merge-base "${BASE}" HEAD 2>/dev/null || echo HEAD)"
echo "Diff base: ${BASE} (merge-base ${MERGE_BASE})"

mapfile -t CHANGED < <(git diff --name-only "${MERGE_BASE}" HEAD)

# C/C++ files anywhere in the diff (for clang-format).
have_cpp_changes() {
  printf '%s\n' "${CHANGED[@]}" | grep -qE '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$'
}

# C/C++ files under our authored src/include trees (for clang-tidy / cppcheck).
mapfile -t SRC_CPP_FILES < <(
  git diff --name-only --diff-filter=ACMR "${MERGE_BASE}" HEAD -- \
    AlpacaCore/src AlpacaCore/include AlpacaHTTP/src AlpacaHTTP/include \
    | grep -E '\.(c|cc|cpp|cxx|h|hpp|hxx)$' || true
)

# Shell scripts we author (mirrors the shellcheck job's selection).
mapfile -t SH_FILES < <(
  {
    printf '%s\n' "${CHANGED[@]}" | grep -E '\.sh$' | grep -v '^AlpacaCore/external/'
    for f in debian/alpacabridge.postinst debian/alpacabridge.postrm debian/alpacabridge.prerm; do
      printf '%s\n' "${CHANGED[@]}" | grep -qx "${f}" && echo "${f}"
    done
  } | sort -u
)

# Hand-written web UI JavaScript (served static, no build step).
mapfile -t JS_FILES < <(
  printf '%s\n' "${CHANGED[@]}" | grep -E '^AlpacaHTTP/web/.*\.js$' || true
)

have_workflow_changes() {
  printf '%s\n' "${CHANGED[@]}" | grep -qE '^\.github/workflows/'
}

# --- gate 1: clang-format (changed lines) ----------------------------------

section "clang-format (changed lines)"
if ! have_cpp_changes; then
  echo "No C/C++ changes -- skipping."
  record SKIP "clang-format (no C/C++ changes)"
elif ensure_tool clang-format clang-format; then
  fmt_diff="$(git-clang-format --commit "${MERGE_BASE}" --diff \
    --extensions c,cc,cpp,cxx,h,hh,hpp,hxx 2>/dev/null)"
  if [ "${fmt_diff}" = "no modified files to format" ] || \
     [ "${fmt_diff}" = "clang-format did not modify any files" ]; then
    echo "Formatting OK."
    record PASS "clang-format"
  else
    echo "${fmt_diff}"
    echo "Fix with: git-clang-format ${MERGE_BASE}"
    record FAIL "clang-format"
  fi
else
  echo "clang-format unavailable -- CI will still enforce it."
  record SKIP "clang-format (tool missing)"
fi

# --- gate 2: unicode / Trojan-Source scan ----------------------------------

section "Unicode / Trojan-Source scan"
if python3 .github/scripts/check-unicode.py; then
  record PASS "unicode scan"
else
  record FAIL "unicode scan"
fi

# --- gate 3: build + unit tests, vendor-neutral ----------------------------

section "Build + tests (vendors OFF)"
if ALPACACORE_ENABLE_ALL_VENDORS=OFF ./run_all_tests.sh; then
  record PASS "build+test (vendors OFF)"
else
  record FAIL "build+test (vendors OFF)"
fi

# --- gate 4: build + unit tests, all vendors -------------------------------

section "Build + tests (vendors ON)"
if ALPACACORE_ENABLE_ALL_VENDORS=ON ./run_all_tests.sh; then
  record PASS "build+test (vendors ON)"
else
  record FAIL "build+test (vendors ON)"
fi

# --- gate 5: clang-tidy (changed lines) ------------------------------------

section "clang-tidy (changed lines)"
if [ "${#SRC_CPP_FILES[@]}" -eq 0 ]; then
  echo "No changed C/C++ source files -- skipping."
  record SKIP "clang-tidy (no source changes)"
elif ensure_tool clang-tidy clang-tidy; then
  # Compile DB covering both trees (AlpacaHTTP pulls AlpacaCore in as a subdir).
  cmake -S AlpacaHTTP -B AlpacaHTTP/build \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DALPACAHTTP_BUILD_TESTS=ON \
    -DALPACACORE_ENABLE_ALL_VENDORS=OFF >/dev/null
  cmake --build AlpacaHTTP/build --parallel "${PARALLEL}" >/dev/null
  tidy_diff="$(dpkg -L clang-tidy 2>/dev/null | grep -m1 -E 'clang-tidy-diff.*\.py' || true)"
  if [ -z "${tidy_diff}" ]; then
    tidy_diff="$(find /usr -name 'clang-tidy-diff*.py' 2>/dev/null | head -1)"
  fi
  if [ -z "${tidy_diff}" ]; then
    echo "clang-tidy-diff.py not found."
    record FAIL "clang-tidy (helper missing)"
  else
    git diff -U0 --no-color "${MERGE_BASE}" -- \
      AlpacaCore/src AlpacaCore/include AlpacaHTTP/src AlpacaHTTP/include \
      | python3 "${tidy_diff}" -p1 -path AlpacaHTTP/build \
          -clang-tidy-binary clang-tidy 2>&1 | tee /tmp/preflight-tidy.log
    if grep -qE ': (warning|error):' /tmp/preflight-tidy.log; then
      record FAIL "clang-tidy"
    else
      echo "clang-tidy OK."
      record PASS "clang-tidy"
    fi
  fi
else
  echo "clang-tidy unavailable -- CI will still enforce it."
  record SKIP "clang-tidy (tool missing)"
fi

# --- gate 6: cppcheck (changed files) --------------------------------------
#
# Keep the --suppress list identical to the cppcheck job in
# .github/workflows/ci.yml. CI builds cppcheck 2.17.x from source to match a
# Debian Trixie dev box, so this gate and CI run the same version; the shared
# suppress list keeps them in agreement (and guards older cppcheck installs,
# which classify some checks differently).

section "cppcheck (changed files)"
if [ "${#SRC_CPP_FILES[@]}" -eq 0 ]; then
  echo "No changed C/C++ source files -- skipping."
  record SKIP "cppcheck (no source changes)"
elif ensure_tool cppcheck cppcheck; then
  if cppcheck \
      --enable=warning,performance,portability \
      --inline-suppr \
      --std=c++20 \
      --language=c++ \
      --error-exitcode=2 \
      --quiet \
      --suppress=missingInclude \
      --suppress=missingIncludeSystem \
      --suppress=normalCheckLevelMaxBranches \
      --suppress=virtualCallInConstructor \
      -I AlpacaCore/include -I AlpacaHTTP/include \
      "${SRC_CPP_FILES[@]}"; then
    echo "cppcheck OK."
    record PASS "cppcheck"
  else
    record FAIL "cppcheck"
  fi
else
  echo "cppcheck unavailable -- CI will still enforce it."
  record SKIP "cppcheck (tool missing)"
fi

# --- gate 7: shellcheck (only if shell scripts changed) --------------------

section "ShellCheck"
if [ "${#SH_FILES[@]}" -eq 0 ]; then
  echo "No authored shell scripts changed -- skipping."
  record SKIP "shellcheck (no shell changes)"
elif ensure_tool shellcheck shellcheck; then
  echo "Linting: ${SH_FILES[*]}"
  if shellcheck "${SH_FILES[@]}"; then
    echo "ShellCheck OK."
    record PASS "shellcheck"
  else
    record FAIL "shellcheck"
  fi
else
  echo "shellcheck unavailable -- CI will still enforce it."
  record SKIP "shellcheck (tool missing)"
fi

# --- gate 8: javascript syntax (only if web JS changed) --------------------

section "JavaScript syntax (node --check)"
if [ "${#JS_FILES[@]}" -eq 0 ]; then
  echo "No web JavaScript changed -- skipping."
  record SKIP "javascript (no JS changes)"
elif ensure_tool node nodejs; then
  js_ok=1
  for f in "${JS_FILES[@]}"; do
    if ! node --check "${f}"; then
      echo "Syntax error in ${f}"
      js_ok=0
    fi
  done
  if [ "${js_ok}" -eq 1 ]; then
    echo "JavaScript syntax OK."
    record PASS "javascript"
  else
    record FAIL "javascript"
  fi
else
  echo "node unavailable -- CI will still check it."
  record SKIP "javascript (tool missing)"
fi

# --- gate 9: zizmor (only if workflows changed) ----------------------------

section "zizmor (workflow audit)"
if ! have_workflow_changes; then
  echo "No workflow changes -- skipping."
  record SKIP "zizmor (no workflow changes)"
else
  zizmor_bin="$(ensure_zizmor)"
  if [ -n "${zizmor_bin}" ]; then
    if "${zizmor_bin}" --offline .github/workflows/; then
      record PASS "zizmor"
    else
      record FAIL "zizmor"
    fi
  else
    echo "zizmor unavailable -- CI will still audit the changed workflow(s)."
    record SKIP "zizmor (tool missing)"
  fi
fi

# --- optional: sanitizers --------------------------------------------------

if [ "${RUN_SANITIZERS:-0}" = "1" ]; then
  section "Sanitizers (ASan + UBSan, vendors OFF)"
  if CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
     LDFLAGS="-fsanitize=address,undefined" \
     ASAN_OPTIONS="abort_on_error=1:detect_leaks=1" \
     UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
     ALPACACORE_ENABLE_ALL_VENDORS=OFF ./run_all_tests.sh; then
    record PASS "sanitizers"
  else
    record FAIL "sanitizers"
  fi
fi

# --- summary ---------------------------------------------------------------

section "Pre-flight summary"
for entry in "${RESULTS[@]}"; do
  printf '  [%-4s] %s\n' "${entry%%|*}" "${entry#*|}"
done

echo
if [ "${OVERALL}" -eq 0 ]; then
  echo "All mandatory checks passed. Safe to push."
else
  echo "One or more checks FAILED -- do not push until fixed."
fi
exit "${OVERALL}"
