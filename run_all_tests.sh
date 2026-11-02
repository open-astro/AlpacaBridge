#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"

# Test-count floors (issue #592). ctest --no-tests=error only fails a run that
# finds ZERO tests; a vendor target that silently stops configuring drops its
# cases and the run still passed (vendors-ON ~800 cases falling to ~80). Each
# run must find at least this many tests. THIS BLOCK is the one place to change
# them: lower a floor only when tests are deliberately removed, raise it when
# the suite grows. Floors sit about 10% below the counts at upstream/main
# 420d74e. Measured: vendors OFF core 202, HTTP 15 (x86_64 `ctest -N`; CI run
# 37869957757 jobs 113625511555 and 113625511686). Vendors ON core 1669, HTTP 16
# (CI run 37869957757 jobs 113625511540 build-vendors and 113625511933
# coverage). A vendors setting with no floor fails loudly.
case "${CORE_VENDORS}" in
  OFF) CORE_FLOOR=180 ; HTTP_FLOOR=13 ;;
  ON)  CORE_FLOOR=1500 ; HTTP_FLOOR=14 ;;
  *)   CORE_FLOOR="" ; HTTP_FLOOR="" ;;
esac

# count_tests <build dir>: prints the `ctest -N` total for a build dir, or
# fails when it cannot be read.
count_tests() {
  local found
  found="$(ctest --test-dir "$1" -N | sed -n 's/^Total Tests: *//p')"
  if [[ ! "${found}" =~ ^[0-9]+$ ]]; then
    return 1
  fi
  echo "${found}"
}

# require_floor <configuration> <found> <floor>: fail when `found` is below
# the floor (or no floor exists for this configuration).
require_floor() {
  local label="$1" found="$2" floor="$3"
  if [[ -z "${floor}" ]]; then
    echo "ERROR: no test-count floor for ${label}; add one at the top of run_all_tests.sh"
    return 1
  fi
  if (( found < floor )); then
    echo "ERROR: ${label} found ${found} tests, below the floor of ${floor}."
    echo "A vendor target or test file probably stopped configuring. If tests were"
    echo "removed on purpose, lower the floor at the top of run_all_tests.sh."
    return 1
  fi
  echo "${label}: ${found} tests (floor ${floor})"
}

# check_floor <configuration> <build dir> <floor>: fail when `ctest -N` finds
# fewer tests than the floor (or no floor exists for this configuration).
check_floor() {
  local label="$1" build_dir="$2" floor="$3" found
  if ! found="$(count_tests "${build_dir}")"; then
    echo "ERROR: could not read the test count for ${label} from 'ctest -N'"
    return 1
  fi
  require_floor "${label}" "${found}" "${floor}"
}

# check_http_floor <configuration> <tree> <core subdir> <floor>: the AlpacaHTTP
# suite is every test in the tree that is not under the AlpacaCore subdir.
check_http_floor() {
  local label="$1" tree="$2" core_dir="$3" floor="$4" total core
  if ! total="$(count_tests "${tree}")" || ! core="$(count_tests "${core_dir}")"; then
    echo "ERROR: could not read the test count for ${label} from 'ctest -N'"
    return 1
  fi
  require_floor "${label}" "$(( total - core ))" "${floor}"
}

# Sourced with RUN_ALL_TESTS_LIB=1 to expose the floor checks for a demonstration.
if [[ "${RUN_ALL_TESTS_LIB:-}" == "1" ]]; then
  # shellcheck disable=SC2317
  return 0 2>/dev/null || exit 0
fi

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi

if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

# AlpacaCore/build is no longer produced here (issue #1251); it is still
# removed so a binary an older run left there cannot be mistaken for this
# run's.
rm -rf "${CORE_DIR}/build" "${HTTP_DIR}/build"

if [[ "${OSTYPE:-}" == "darwin"* ]]; then
  PARALLEL="$(sysctl -n hw.ncpu)"
elif command -v nproc >/dev/null 2>&1; then
  PARALLEL="$(nproc)"
else
  PARALLEL="4"
fi

# ctest gets its own parallelism, deliberately above the core count. The suite
# is dominated by driver cases that wait on fake hardware in real time rather
# than computing: the slowest single case (a SkyWatcher pole slew) measured
# 110.5s wall for 0.13s user + 0.32s sys, so pinning ctest to nproc leaves the
# machine idle. Measured on ubuntu-24.04-arm (4 cores, the CI runner), all
# vendors, 802 cases: -j 4 => 488.9s, -j 8 => 252.0s.
#
# Twice the core count rather than a fixed number, because what governs is the
# oversubscription RATIO, not the absolute -j: a fixed 8 would carry a 4x ratio
# onto any 2-core box, while 2x nproc does not.
#
# Oversubscription is what surfaces a test whose assertion depends on the
# scheduler rather than on behaviour. `HostClock - readers in flight survive a
# concurrent set_hooks` was one: its reader threads checked the stop flag before
# their first read, so under enough contention the writer loop finished before
# any reader was scheduled and the `reads > 0` vacuity guard went red on an
# otherwise healthy tree -- 3 runs in 10 at -j 8 under ASan+UBSan on a 4-core
# box. That case now starts each reader with one unconditional read and waits
# for all four before the writer loop, which is a property of the case and not
# of the -j it runs at. Raising this multiplier further is a separate change
# and wants its own measurement: the numbers above are all-vendors and the
# -j 8 evidence for that fix is vendors-OFF under sanitizers.
#
# CTEST_PARALLEL overrides, so a slower or busier machine can dial it back
# without a code change. The BUILD stays at nproc below: compiling is CPU-bound.
CTEST_PARALLEL="${CTEST_PARALLEL:-$((PARALLEL * 2))}"

# Per-test timeout, so one hang fails that test by name instead of burning the
# job's whole time limit (issue #1796). Measured on the nightly ASan + UBSan
# job (all vendors, 3718 cases, 4 cores, CI run 40665928481): the slowest
# passing test took 274.85 s ("OnStep telescope - Park and FindHome race the
# getters and a reconnect") and test_routing was at 299.67 s when a leak
# aborted it. 900 s is 3x the slowest of them. CTEST_TIMEOUT overrides.
CTEST_TIMEOUT="${CTEST_TIMEOUT:-900}"

# One tree builds both suites (issue #1251). AlpacaHTTP adds AlpacaCore as a
# subdirectory (AlpacaHTTP/CMakeLists.txt), so configuring it with
# ALPACACORE_BUILD_TESTS=ON also adds AlpacaCore/tests under
# AlpacaHTTP/build/AlpacaCore and compiles every AlpacaCore source once. The
# old layout built AlpacaCore/build first and then AlpacaHTTP/build with the
# core tests OFF, which compiled the core library a second time.
BUILD_DIR="${HTTP_DIR}/build"
CORE_BUILD_DIR="${BUILD_DIR}/AlpacaCore"

echo "== AlpacaCore + AlpacaHTTP =="
cmake -S "${HTTP_DIR}" -B "${BUILD_DIR}" \
  -DALPACAHTTP_BUILD_TESTS=ON \
  -DALPACACORE_BUILD_TESTS=ON \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${BUILD_DIR}" --parallel "${PARALLEL}"
# Floors per suite: ctest -N in the AlpacaCore subdirectory counts the core
# cases alone, and the whole tree counts both, so the HTTP count is the
# difference.
check_floor "AlpacaCore (vendors ${CORE_VENDORS})" "${CORE_BUILD_DIR}" "${CORE_FLOOR}"
check_http_floor "AlpacaHTTP (vendors ${CORE_VENDORS})" "${BUILD_DIR}" "${CORE_BUILD_DIR}" "${HTTP_FLOOR}"
# --no-tests=error: ctest exits 0 when it finds NO tests, so a suite that
# silently failed to configure (Catch2 missing => AlpacaCore/tests/
# CMakeLists.txt returns early) passed vacuously here, in CI and in
# ci_preflight.sh alike (issue #586). Requires CMake >= 3.18. It catches only an
# EMPTY tree; the floors above catch a shrunken suite (issue #592). One ctest
# over the whole tree runs both suites in one parallel pool.
if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure --no-tests=error --timeout "${CTEST_TIMEOUT}" -j "${CTEST_PARALLEL}"; then
  # CTEST_RERUN_FAILED=1 (the nightly jobs set it): the failed cases run once
  # more, alone and serially, and that run is the verdict. The all-vendors
  # suite is ~3800 cases that wait on fake hardware in real time; at 2x nproc
  # under ASan + UBSan a different case lost a scheduler race on five nights
  # out of seven (runs 40665928481, 40775992105, 40853696579, 40862920401,
  # 40958614341), and each per-case fix was followed by the next case. A
  # sanitizer finding is deterministic and fails again here; a case that only
  # loses under oversubscription passes, and the first run's output above
  # still names it. PR CI keeps the single run: the oversubscription is what
  # surfaces a scheduler-dependent assertion (see CTEST_PARALLEL above).
  [ "${CTEST_RERUN_FAILED:-0}" = "1" ] || exit 1
  echo "== Rerunning the failed tests alone (-j 1); this run is the verdict =="
  ctest --test-dir "${BUILD_DIR}" --rerun-failed --output-on-failure --timeout "${CTEST_TIMEOUT}" -j 1
fi
