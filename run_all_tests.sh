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

# check_floor <configuration> <build dir> <floor>: fail when `ctest -N` finds
# fewer tests than the floor (or no floor exists for this configuration).
check_floor() {
  local label="$1" build_dir="$2" floor="$3" found
  if [[ -z "${floor}" ]]; then
    echo "ERROR: no test-count floor for ${label}; add one at the top of run_all_tests.sh"
    return 1
  fi
  found="$(ctest --test-dir "${build_dir}" -N | sed -n 's/^Total Tests: *//p')"
  if [[ ! "${found}" =~ ^[0-9]+$ ]]; then
    echo "ERROR: could not read the test count for ${label} from 'ctest -N'"
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

# Sourced with RUN_ALL_TESTS_LIB=1 to expose check_floor for a demonstration.
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

echo "== AlpacaCore =="
cmake -S "${CORE_DIR}" -B "${CORE_DIR}/build" \
  -DALPACACORE_BUILD_TESTS=ON \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${CORE_DIR}/build" --parallel "${PARALLEL}"
check_floor "AlpacaCore (vendors ${CORE_VENDORS})" "${CORE_DIR}/build" "${CORE_FLOOR}"
# --no-tests=error: ctest exits 0 when it finds NO tests, so a suite that
# silently failed to configure (Catch2 missing => AlpacaCore/tests/
# CMakeLists.txt returns early) passed vacuously here, in CI and in
# ci_preflight.sh alike (issue #586). Requires CMake >= 3.18. It catches only an
# EMPTY suite; check_floor above catches a shrunken one (issue #592).
ctest --test-dir "${CORE_DIR}/build" --output-on-failure --no-tests=error -j "${CTEST_PARALLEL}"

echo "== AlpacaHTTP =="
# AlpacaHTTP adds AlpacaCore as a subdirectory (AlpacaHTTP/CMakeLists.txt),
# and ALPACACORE_BUILD_TESTS defaults ON -- so without this the whole core
# suite is configured, built and run a SECOND time here (issue #586 made
# that visible: 802 cases in the core run, then 809 = 802 + 7 again).
cmake -S "${HTTP_DIR}" -B "${HTTP_DIR}/build" \
  -DALPACAHTTP_BUILD_TESTS=ON \
  -DALPACACORE_BUILD_TESTS=OFF \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${HTTP_DIR}/build" --parallel "${PARALLEL}"
check_floor "AlpacaHTTP (vendors ${CORE_VENDORS})" "${HTTP_DIR}/build" "${HTTP_FLOOR}"
ctest --test-dir "${HTTP_DIR}/build" --output-on-failure --no-tests=error -j "${CTEST_PARALLEL}"
