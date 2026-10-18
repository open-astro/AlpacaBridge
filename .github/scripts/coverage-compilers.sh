#!/usr/bin/env bash
# Writes gcc/g++ wrappers that add -fprofile-update=atomic and exports them as
# CC/CXX for the rest of the job.
#
# The [stress] cases are multi-threaded, so gcov counters must be updated
# atomically. ccache 4.9 rejects -fprofile-update=atomic as an "Unknown
# profiling option" and caches nothing (4888 of 4888 compiles uncacheable,
# run 39102485820), so the option goes in a wrapper ccache never sees. Pair it
# with CCACHE_COMPILERCHECK=content: the wrapper records the real compiler's
# version line and sha256, so any change to the compiler binary starts a fresh
# cache.
set -euo pipefail

wrap="${RUNNER_TEMP:?}/coverage-cc"
mkdir -p "${wrap}"
for cc in gcc g++; do
  real="$(command -v "${cc}")"
  {
    echo '#!/bin/sh'
    echo "# $("${real}" --version | head -n1)"
    echo "# sha256 $(sha256sum "$(readlink -f "${real}")" | cut -d' ' -f1)"
    echo "exec ${real} -fprofile-update=atomic \"\$@\""
  } > "${wrap}/${cc}"
  chmod +x "${wrap}/${cc}"
  cat "${wrap}/${cc}"
done
{
  echo "CC=${wrap}/gcc"
  echo "CXX=${wrap}/g++"
} >> "${GITHUB_ENV:?}"
