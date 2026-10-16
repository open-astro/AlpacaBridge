#!/usr/bin/env bash
# Acceptance checks for the bundled libgphoto2, run against an INSTALLED
# alpacabridge package (CI `package-deb` job, or the rig):
#
#   scripts/check_bundled_gphoto.sh [--pid N]
#
# Each check prints PASS or FAIL with the raw command output; the exit status is
# non-zero when any check failed. --pid N (rig use) additionally checks that the
# running server mapped libgphoto2 only from the bundle (needs sudo).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
eval "$("${REPO_ROOT}/scripts/build_bundled_gphoto.sh" --print-pins)"

SERVER=/usr/bin/alpacabridge
BUNDLE_LIB="${GPHOTO_PREFIX}/lib"
PTP2="$(find "${BUNDLE_LIB}/libgphoto2" -name ptp2.so 2>/dev/null | head -n 1)"
RULES=/usr/lib/udev/rules.d/61-alpacabridge-libgphoto2.rules
PID=""
ALLOWED_RPATH_ROOT=/usr/lib/alpacabridge

while [ $# -gt 0 ]; do
  case "$1" in
    --pid)
      if [ $# -lt 2 ] || [ -z "$2" ]; then
        echo "--pid needs a process id" >&2
        exit 2
      fi
      PID="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

failed=0
indent() { sed 's/^/    /'; }
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; failed=1; }

# Every libgphoto2 library `ldd` resolves for $1 must live under the bundle, and
# both sonames must be present.
check_ldd() {
  local target="$1" out bad
  if [ ! -e "${target}" ]; then fail "ldd ${target}: file is missing"; return; fi
  out="$(ldd "${target}" 2>&1)"
  echo "${out}" | grep -i 'libgphoto2' | sed 's/^/    /'
  bad="$(echo "${out}" | grep -i 'libgphoto2' | grep -vF "${BUNDLE_LIB}/" || true)"
  if echo "${out}" | grep -q 'libgphoto2.so.6 => '"${BUNDLE_LIB}"/ \
     && echo "${out}" | grep -q 'libgphoto2_port.so.12 => '"${BUNDLE_LIB}"/ \
     && [ -z "${bad}" ]; then
    pass "ldd ${target}: libgphoto2.so.6 and libgphoto2_port.so.12 resolve under ${BUNDLE_LIB}/"
  else
    fail "ldd ${target}: libgphoto2 resolves outside ${BUNDLE_LIB}/ or is missing"
  fi
}

# A library or camlib does not list libgphoto2.so.6 as its own dependency (ldd of
# libgphoto2.so.6 omits itself), so require only libgphoto2_port.
check_ldd_port() {
  local target="$1" out bad
  if [ -z "${target}" ] || [ ! -e "${target}" ]; then fail "ldd of ${target:-the camlib}: file is missing"; return; fi
  out="$(ldd "${target}" 2>&1)"
  echo "${out}" | grep -i 'libgphoto2' | sed 's/^/    /'
  bad="$(echo "${out}" | grep -i 'libgphoto2' | grep -vF "${BUNDLE_LIB}/" || true)"
  if echo "${out}" | grep -q 'libgphoto2_port.so.12 => '"${BUNDLE_LIB}"/ && [ -z "${bad}" ]; then
    pass "ldd ${target}: libgphoto2 libraries resolve under ${BUNDLE_LIB}/"
  else
    fail "ldd ${target}: libgphoto2 resolves outside ${BUNDLE_LIB}/ or is missing"
  fi
}

# The dynamic section must carry a DT_RPATH naming the bundle (not only
# DT_RUNPATH, which the loader does not apply to transitive lookups) and no
# RPATH/RUNPATH entry outside /usr/lib/alpacabridge.
check_rpath() {
  local target="$1" out entries stray
  if [ ! -e "${target}" ]; then fail "readelf ${target}: file is missing"; return; fi
  out="$(readelf -d "${target}" 2>&1)"
  entries="$(echo "${out}" | grep -E '\((RPATH|RUNPATH)\)' || true)"
  indent <<< "${entries}"
  # Split on ':' keeping empty entries: glibc reads an empty entry as the current
  # directory. An entry with '..' could climb out of the prefix.
  stray="$(echo "${entries}" | grep -oE '\[[^]]*\]' | tr -d '[]' \
           | awk -F: -v pre="${ALLOWED_RPATH_ROOT}" '
               function inside(d) { return index(d, pre) == 1 && (length(d) == length(pre) || substr(d, length(pre) + 1, 1) == "/") }
               $0 == "" { print "<empty>"; next }
               { for (i = 1; i <= NF; i++)
                   if ($i == "") print "<empty>"
                   else if ($i ~ /\.\./ || !inside($i)) print $i }
               END { print "@@end" }')"
  # A failed awk prints no sentinel: treat that as a stray entry, never as clean.
  if [ "${stray##*$'\n'}" = "@@end" ]; then stray="${stray%@@end}"; stray="${stray%$'\n'}"
  else stray="<could not parse the dynamic section>"; fi
  if echo "${entries}" | grep '(RPATH)' | grep -qF "${BUNDLE_LIB}" && [ -z "${stray}" ]; then
    pass "readelf -d ${target}: (RPATH) names ${BUNDLE_LIB}, nothing outside /usr/lib/alpacabridge"
  else
    fail "readelf -d ${target}: no (RPATH) naming ${BUNDLE_LIB}, or a stray entry: ${stray}"
  fi
}

# 1. The server resolves libgphoto2 from the bundle.
check_ldd "${SERVER}"
# 2. The bundle's own library and camlib do too.
check_ldd_port "${BUNDLE_LIB}/libgphoto2.so.6"
check_ldd_port "${PTP2}"
# 3. The loader path is a DT_RPATH.
check_rpath "${SERVER}"
check_rpath "${BUNDLE_LIB}/libgphoto2.so.6"

# 4. udev rules generated from the bundled build.
if [ -f "${RULES}" ] \
   && grep -q '\*:060101:\*' "${RULES}" && grep -q 'GROUP="plugdev"' "${RULES}" \
   && head -n 3 "${RULES}" | grep -qF "libgphoto2 ${LIBGPHOTO2_VER}"; then
  pass "${RULES}: PTP interface-class line, GROUP=\"plugdev\", header names libgphoto2 ${LIBGPHOTO2_VER}"
else
  fail "${RULES}: missing, or no *:060101:* line, no GROUP=\"plugdev\", or no libgphoto2 ${LIBGPHOTO2_VER} header"
  head -n 3 "${RULES}" 2>&1 | sed 's/^/    /'
fi

# 5. The package no longer depends on the distro's libgphoto2.
if depends="$(dpkg-query -W -f '${Depends}' alpacabridge 2>&1)"; then dpkg_ok=1; else dpkg_ok=0; fi
echo "    Depends: ${depends}"
if [ "${dpkg_ok}" -eq 0 ]; then
  fail "dpkg-query could not read the alpacabridge package: ${depends}"
elif echo "${depends}" | grep -qE 'libgphoto2-6|libgphoto2-port12'; then
  fail "dpkg-query Depends names a distro libgphoto2 package"
else
  pass "dpkg-query Depends names neither libgphoto2-6t64 nor libgphoto2-port12t64"
fi

# 5b. The bundle is built without libxml2 (--with-libxml-2.0=no): it only served the
#     experimental Olympus E-series XML-wrapped mode and the Wi-Fi-only lumix camlib,
#     neither reachable through USB autodetect, and it carried an unfixed Critical CVE.
# (-N) also matches a soname-versioned package name such as libxml2-16, not libxml2-utils or libxml2-dev.
if [ "${dpkg_ok}" -eq 1 ] && echo "${depends}" | grep -qE '(^|[ ,])libxml2(-[0-9]+)?([ (,]|$)'; then
  fail "dpkg-query Depends names libxml2"
elif [ "${dpkg_ok}" -eq 1 ]; then
  pass "dpkg-query Depends does not name libxml2"
fi
xml_hits=""
scanned=0
while IFS= read -r so; do
  scanned=$((scanned + 1))
  if ldd "${so}" 2>&1 | grep -q 'libxml2\.so'; then xml_hits+="${so}"$'\n'; fi
done < <(find "${BUNDLE_LIB}" -name '*.so*' -type f 2>/dev/null)
if [ "${scanned}" -eq 0 ]; then
  # A missing bundle or a wrong prefix would otherwise pass this check having read nothing.
  fail "ldd: no shared library found under ${BUNDLE_LIB} to check for libxml2"
elif [ -n "${xml_hits}" ]; then
  indent <<< "${xml_hits%$'\n'}"
  fail "ldd: a bundled library or camlib links libxml2"
else
  pass "ldd: no bundled library or camlib links libxml2"
fi

# 6. The bundle directory is not registered with ldconfig.
shopt -s nullglob
confs=(/etc/ld.so.conf /etc/ld.so.conf.d/*)
shopt -u nullglob
hits="$(grep -n 'libgphoto2' "${confs[@]}" 2>/dev/null || true)"
if [ -n "${hits}" ]; then
  indent <<< "${hits}"
  fail "ld.so.conf mentions libgphoto2: the bundle would shadow the distro library for every program"
else
  pass "no ld.so.conf entry mentions libgphoto2"
fi

# 7. The bundle is linked with full RELRO (BIND_NOW), as the distro libgphoto2 is:
#    it parses data from untrusted USB devices.
for lib in "${BUNDLE_LIB}/libgphoto2.so.6" "${BUNDLE_LIB}/libgphoto2_port.so.12" "${PTP2}"; do
  if [ -z "${lib}" ] || [ ! -e "${lib}" ]; then fail "readelf -d ${lib:-the camlib}: file is missing"; continue; fi
  flags="$(readelf -dW "${lib}" 2>&1 | grep -E '\((FLAGS|FLAGS_1|BIND_NOW)\)' || true)"
  indent <<< "${flags:-(no FLAGS entry)}"
  if grep -qE '\(BIND_NOW\)|\(FLAGS\).*BIND_NOW|\(FLAGS_1\).*NOW' <<< "${flags}"; then
    pass "readelf -d ${lib}: BIND_NOW (full RELRO)"
  else
    fail "readelf -d ${lib}: no BIND_NOW, so only partial RELRO"
  fi
done

# 8. The package ships no build-only file under the bundle prefix.
if files="$(dpkg -L alpacabridge 2>&1)"; then
  extra="$(grep -E "^${GPHOTO_PREFIX}/(bin|include|lib/pkgconfig|lib/udev)(/|\$)|^${GPHOTO_PREFIX}/lib/libgphoto2(_port)?\.so\$" <<< "${files}" || true)"
  if [ -n "${extra}" ]; then
    indent <<< "${extra}"
    fail "dpkg -L alpacabridge: build-only files under ${GPHOTO_PREFIX}/"
  else
    pass "dpkg -L alpacabridge: no bin/, include/, pkgconfig/, udev helper or unversioned .so under ${GPHOTO_PREFIX}/"
  fi
else
  fail "dpkg -L could not read the alpacabridge package: ${files}"
fi

# 9. Rig use: the running server maps libgphoto2 only from the bundle.
if [ -n "${PID}" ]; then
  # Reading another user's maps needs root; report that, not a missing mapping.
  if ! raw="$(cat "/proc/${PID}/maps" 2>&1)"; then
    indent <<< "${raw}"
    fail "/proc/${PID}/maps: cannot read it (run this script with sudo for --pid)"
  else
    maps="$(grep libgphoto2 <<< "${raw}" | awk '{print $6}' | sort -u)"
    indent <<< "${maps}"
    if [ -n "${maps}" ] && ! echo "${maps}" | grep -vF "${GPHOTO_PREFIX}/" | grep -q .; then
      pass "/proc/${PID}/maps: libgphoto2 mapped only from ${GPHOTO_PREFIX}/"
    else
      fail "/proc/${PID}/maps: no libgphoto2 mapping, or one outside ${GPHOTO_PREFIX}/"
    fi
  fi
fi

exit "${failed}"
