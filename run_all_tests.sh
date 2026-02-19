#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CORE_DIR="${ROOT_DIR}/AlpacaCore"
HTTP_DIR="${ROOT_DIR}/AlpacaHTTP"
HTTP_BEAST="${ALPACAHTTP_USE_BOOST_BEAST:-OFF}"
CORE_VENDORS="${ALPACACORE_ENABLE_ALL_VENDORS:-ON}"
ENABLE_VCPKG="${ALPACABRIDGE_ENABLE_VCPKG:-ON}"
BUILD_CONFIG="${ALPACA_BUILD_CONFIG:-Debug}"
IS_WINDOWS_BASH=0
CMAKE_EXTRA_ARGS=()
BUILD_EXTRA_ARGS=()
CTEST_EXTRA_ARGS=()

case "${OSTYPE:-}" in
  msys*|cygwin*) IS_WINDOWS_BASH=1 ;;
esac

if [[ ! -d "${CORE_DIR}" ]]; then
  echo "AlpacaCore not found at ${CORE_DIR}"
  exit 1
fi

if [[ ! -d "${HTTP_DIR}" ]]; then
  echo "AlpacaHTTP not found at ${HTTP_DIR}"
  exit 1
fi

if [[ "${IS_WINDOWS_BASH}" -eq 1 ]]; then
  CORE_BUILD_WIN="$(cygpath -w "${CORE_DIR}/build")"
  HTTP_BUILD_WIN="$(cygpath -w "${HTTP_DIR}/build")"
  cmd.exe /c "if exist \"${CORE_BUILD_WIN}\" rmdir /s /q \"${CORE_BUILD_WIN}\""
  cmd.exe /c "if exist \"${HTTP_BUILD_WIN}\" rmdir /s /q \"${HTTP_BUILD_WIN}\""
else
  rm -rf "${CORE_DIR}/build" "${HTTP_DIR}/build"
fi

if [[ "${OSTYPE:-}" == "darwin"* ]]; then
  PARALLEL="$(sysctl -n hw.ncpu)"
else
  if command -v nproc >/dev/null 2>&1; then
    PARALLEL="$(nproc)"
  else
    PARALLEL="4"
  fi
fi

if [[ "${IS_WINDOWS_BASH}" -eq 1 && "${ENABLE_VCPKG}" == "ON" ]]; then
  if [[ -z "${VCPKG_ROOT:-}" ]]; then
    if [[ -n "${USERPROFILE:-}" ]]; then
      VCPKG_ROOT="$(cygpath -u "${USERPROFILE}")/vcpkg"
    else
      VCPKG_ROOT="${HOME}/vcpkg"
    fi
  fi

  if [[ ! -x "${VCPKG_ROOT}/vcpkg.exe" ]]; then
    if ! command -v git >/dev/null 2>&1; then
      echo "Git is required to bootstrap vcpkg."
      exit 1
    fi
    if [[ ! -d "${VCPKG_ROOT}" ]]; then
      echo "== Installing vcpkg to ${VCPKG_ROOT} =="
      git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}"
    fi
    echo "== Bootstrapping vcpkg =="
    cmd.exe /c "\"$(cygpath -w "${VCPKG_ROOT}")\\bootstrap-vcpkg.bat\""
  fi

  VCPKG_TRIPLET="${VCPKG_DEFAULT_TRIPLET:-x64-windows}"
  CMAKE_EXTRA_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$(cygpath -m "${VCPKG_ROOT}")/scripts/buildsystems/vcpkg.cmake")
  CMAKE_EXTRA_ARGS+=("-DVCPKG_MANIFEST_DIR=$(cygpath -m "${ROOT_DIR}")")
  CMAKE_EXTRA_ARGS+=("-DVCPKG_TARGET_TRIPLET=${VCPKG_TRIPLET}")
  BUILD_EXTRA_ARGS+=(--config "${BUILD_CONFIG}")
  CTEST_EXTRA_ARGS+=(-C "${BUILD_CONFIG}")
  echo "== Using vcpkg ${VCPKG_TRIPLET} =="
fi

echo "== AlpacaCore =="
cmake -S "${CORE_DIR}" -B "${CORE_DIR}/build" \
  "${CMAKE_EXTRA_ARGS[@]}" \
  -DALPACACORE_BUILD_TESTS=ON \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${CORE_DIR}/build" "${BUILD_EXTRA_ARGS[@]}" --parallel "${PARALLEL}"
ctest --test-dir "${CORE_DIR}/build" "${CTEST_EXTRA_ARGS[@]}" --output-on-failure -j "${PARALLEL}"

echo "== AlpacaHTTP =="
cmake -S "${HTTP_DIR}" -B "${HTTP_DIR}/build" \
  "${CMAKE_EXTRA_ARGS[@]}" \
  -DALPACAHTTP_BUILD_TESTS=ON \
  -DALPACAHTTP_USE_BOOST_BEAST="${HTTP_BEAST}" \
  -DALPACACORE_ENABLE_ALL_VENDORS="${CORE_VENDORS}"
cmake --build "${HTTP_DIR}/build" "${BUILD_EXTRA_ARGS[@]}" --parallel "${PARALLEL}"
ctest --test-dir "${HTTP_DIR}/build" "${CTEST_EXTRA_ARGS[@]}" --output-on-failure -j "${PARALLEL}"
