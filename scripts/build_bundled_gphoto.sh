#!/usr/bin/env bash
# Build the vendored libgphoto2 (AlpacaCore/external/libgphoto2/) into a private
# prefix, so AlpacaBridge neither links nor loads the distro's libgphoto2.
#
#   scripts/build_bundled_gphoto.sh [--destdir DIR] [--build-dir DIR] [--jobs N]
#   scripts/build_bundled_gphoto.sh --print-pins
#
# Installs <DESTDIR>/usr/lib/alpacabridge/libgphoto2/{include,lib} and
# <DESTDIR>/usr/lib/udev/rules.d/61-alpacabridge-libgphoto2.rules. Without
# --destdir it installs into the real prefix (needs root). Without --build-dir
# it builds in a temporary directory that is removed on exit, so a sudo run
# leaves no root-owned tree in the checkout.
#
# The pins below are the single source of truth: CI and debian/rules run this
# script, and the acceptance script and the two installers read them through
# --print-pins. The README beside the
# tarball records the same values (bump procedure there).
#
# The prefix is compiled into libgphoto2 (camlib and iolib directories), and the
# library carries a DT_RPATH (not DT_RUNPATH) to its own lib directory, so the
# loader resolves libgphoto2_port.so.12 from the bundle even when the distro
# ships the same soname. The directory is never registered with ldconfig.
set -euo pipefail

LIBGPHOTO2_VER=2.5.34
LIBGPHOTO2_SHA256=51993f5d9bfb6b4e5925cbbe5883085791bff6f81bcacb8ffe1b783ce76d586a
GPHOTO_PREFIX=/usr/lib/alpacabridge/libgphoto2

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARBALL="${REPO_ROOT}/AlpacaCore/external/libgphoto2/libgphoto2-${LIBGPHOTO2_VER}.tar.xz"
RULES_NAME=61-alpacabridge-libgphoto2.rules

DESTDIR=""
BUILD_DIR=""
JOBS="$(nproc)"

die() { echo "build_bundled_gphoto.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --print-pins)
      printf 'LIBGPHOTO2_VER=%s\nLIBGPHOTO2_SHA256=%s\nGPHOTO_PREFIX=%s\n' \
        "${LIBGPHOTO2_VER}" "${LIBGPHOTO2_SHA256}" "${GPHOTO_PREFIX}"
      exit 0 ;;
    --destdir) [ $# -ge 2 ] || die "--destdir needs a directory"; DESTDIR="$2"; shift 2 ;;
    --build-dir) [ $# -ge 2 ] || die "--build-dir needs a directory"; BUILD_DIR="$2"; shift 2 ;;
    --jobs) [ $# -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    -h|--help) echo "usage: $0 [--destdir DIR] [--build-dir DIR] [--jobs N] | --print-pins"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# --- Step 1: verify the vendored tarball before anything is extracted ---------
[ -f "${TARBALL}" ] || die "missing ${TARBALL}"
echo "${LIBGPHOTO2_SHA256}  ${TARBALL}" | sha256sum -c - \
  || die "sha256 mismatch for ${TARBALL}: refusing to extract"

if [ -n "${DESTDIR}" ]; then
  mkdir -p "${DESTDIR}"
  DESTDIR="$(cd "${DESTDIR}" && pwd)"
fi
if [ -z "${BUILD_DIR}" ]; then
  BUILD_DIR="$(mktemp -d)"
  trap 'rm -rf "${BUILD_DIR}"' EXIT
fi
SRC_DIR="${BUILD_DIR}/libgphoto2-${LIBGPHOTO2_VER}"
STAGE_PREFIX="${DESTDIR}${GPHOTO_PREFIX}"

# --- Step 2: build and install the library, camlibs and iolibs ----------------
rm -rf "${SRC_DIR}"
mkdir -p "${BUILD_DIR}"
tar -xf "${TARBALL}" -C "${BUILD_DIR}"
[ -x "${SRC_DIR}/configure" ] || die "${SRC_DIR}/configure not found after extraction"

(
  cd "${SRC_DIR}"
  # --disable-new-dtags makes the rpath a DT_RPATH, which the loader applies to
  # transitive lookups, so libgphoto2.so.6 finds its own libgphoto2_port.so.12.
  # -z relro -z now gives full RELRO, as Debian's own libgphoto2 build has; the
  # caller's LDFLAGS (debian/rules exports dpkg-buildflags) are kept in front.
  ./configure --prefix="${GPHOTO_PREFIX}" --disable-static --disable-nls --disable-docs \
    --with-camlibs=standard --with-libexif=auto --with-libxml-2.0=auto \
    --with-libcurl=no --with-gdlib=no --with-libtiff-4=no \
    LDFLAGS="${LDFLAGS:-} -Wl,-z,relro -Wl,-z,now -Wl,-rpath,${GPHOTO_PREFIX}/lib -Wl,--disable-new-dtags"
  make -j"${JOBS}"
  make install DESTDIR="${DESTDIR}"
)

# --- Step 3: udev rules generated from the staged build ----------------------
# print-camera-list carries a DT_RPATH to the final prefix, which beats
# LD_LIBRARY_PATH: on a host that already has the bundle installed it would load
# the installed library, not the staged one. Run it through the dynamic loader
# with the RPATH of the executable and of the two staged libraries inhibited and
# the stage as the library path, then refuse to continue if it still resolves
# libgphoto2 anywhere else.
PCL="${STAGE_PREFIX}/lib/libgphoto2/print-camera-list"
[ -x "${PCL}" ] || die "${PCL} was not installed"
STAGE_LIB="${STAGE_PREFIX}/lib"
LOADER="$(readelf -l "${PCL}" | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')"
[ -x "${LOADER}" ] || die "cannot find the dynamic loader of ${PCL} (got '${LOADER}')"
# The empty first entry names the main program.
INHIBIT=":${STAGE_LIB}/libgphoto2.so.6:${STAGE_LIB}/libgphoto2_port.so.12"
run_pcl() {
  "${LOADER}" --inhibit-rpath "${INHIBIT}" --library-path "${STAGE_LIB}" "${PCL}" "$@"
}
resolved="$(LD_TRACE_LOADED_OBJECTS=1 "${LOADER}" --inhibit-rpath "${INHIBIT}" \
  --library-path "${STAGE_LIB}" "${PCL}" | awk '$1 == "libgphoto2.so.6" {print $3}')"
real_resolved="$(readlink -f "${resolved:-/nonexistent}")"
real_stage="$(readlink -f "${STAGE_LIB}")"
case "${real_resolved}" in
  "${real_stage}"/*) ;;
  *) die "print-camera-list loads ${resolved:-nothing} instead of the staged library under ${STAGE_LIB}" ;;
esac

rules_dir="${DESTDIR}/usr/lib/udev/rules.d"
mkdir -p "${rules_dir}"
rules="${rules_dir}/${RULES_NAME}"
{
  echo "# Generated by scripts/build_bundled_gphoto.sh from libgphoto2 ${LIBGPHOTO2_VER}"
  CAMLIBS="${STAGE_PREFIX}/lib/libgphoto2/${LIBGPHOTO2_VER}" \
  IOLIBS="$(echo "${STAGE_PREFIX}"/lib/libgphoto2_port/*)" \
    run_pcl udev-rules version 201 group plugdev mode 0660
} > "${rules}.tmp"
mv "${rules}.tmp" "${rules}"

grep -q '\*:060101:\*' "${rules}" || die "${rules}: no PTP interface-class (*:060101:*) line"
grep -q 'GROUP="plugdev"' "${rules}" || die "${rules}: no GROUP=\"plugdev\" assignment"
[ "$(wc -l < "${rules}")" -gt 20 ] || die "${rules}: fewer than 21 lines, the camera list is missing"

# --- Step 4: drop what the package must not ship ------------------------------
find "${STAGE_PREFIX}/lib" -name '*.la' -delete
rm -rf "${STAGE_PREFIX}/share"
# debian/rules ships the bundle unstripped (dh_strip excludes usr/lib/alpacabridge/),
# so drop the debug info here; --strip-unneeded keeps the dynamic section and RPATH.
if command -v strip >/dev/null 2>&1; then
  find "${STAGE_PREFIX}/lib" -type f \( -name '*.so*' -o -name print-camera-list \) \
    -exec strip --strip-unneeded {} +
fi

echo "libgphoto2 ${LIBGPHOTO2_VER} installed under ${STAGE_PREFIX}; rules in ${rules}"
