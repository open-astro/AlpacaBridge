# libgphoto2 2.5.34 source tarball (vendored, built into the .deb)

`libgphoto2-2.5.34.tar.xz` — pristine upstream release tarball, published at
`https://github.com/gphoto/libgphoto2/releases/download/v2.5.34/libgphoto2-2.5.34.tar.xz`
(7,086,120 bytes).

sha256: `51993f5d9bfb6b4e5925cbbe5883085791bff6f81bcacb8ffe1b783ce76d586a`
(hashed locally after downloading from the release page; the release also ships
a detached `libgphoto2-2.5.34.tar.xz.asc` signature, which was downloaded but
**not verified**: no gphoto signing key is pinned in this repository).

**Why bundled:** Debian trixie ships libgphoto2 2.5.31-4, which lacks the fixes
for CVE-2026-40333, -40334, -40335, -40336, -40338, -40339, -40340 and -40341
(fixed in 2.5.34 per its NEWS; Debian rates them `no-dsa`). The gphoto camera
driver is the only user of the library, so AlpacaBridge builds this tarball into
a private prefix, `/usr/lib/alpacabridge/libgphoto2`, instead of waiting for
Debian. The host's own libgphoto2 packages are untouched. Rationale and the
loader design: [decision 0012](../../../docs/decisions/0012-bundled-libgphoto2.md).

**How it is built:** `scripts/build_bundled_gphoto.sh` verifies the tarball
against the pin, runs `configure --prefix=/usr/lib/alpacabridge/libgphoto2` with
the default `standard` camlib set, installs, and generates
`61-alpacabridge-libgphoto2.rules` from the built camera list. No network fetch
anywhere in the build. `debian/rules` runs it for the .deb; CI, `build_and_run.sh`
and `install_alpaca_service.sh` run it for development builds.

**What the checksum does and does not protect:** with the tarball and the pin in
the same repository, `sha256sum -c` guards against *corruption* (a bad checkout,
filter mangling, bit-rot), NOT against adversarial modification, since one commit
can swap both the binary and the pin. Review changes to this directory like a
dependency bump: re-download from the URL above, re-hash, and compare.

**Licence:** LGPL-2.1-or-later (`COPYING` in the tarball); some camlibs carry
their own GPL-2.0-or-later `COPYING`. See the stanza in `debian/copyright`.

## Bumping the version

Update all of these in one commit:

1. The tarball here (delete the old one) and this README (URL, size, sha256).
2. `LIBGPHOTO2_VER` and `LIBGPHOTO2_SHA256` in `scripts/build_bundled_gphoto.sh`
   (the only pin in code: CI and `debian/rules` run the script, and
   `scripts/check_bundled_gphoto.sh`, `build_and_run.sh` and
   `install_alpaca_service.sh` read it through `--print-pins`).
3. The `AlpacaCore/external/libgphoto2` entry in `scripts/dependency_sdk_versions.json`
   (`version`, `source`), then `git add` the files and run
   `python3 scripts/dependency_snapshot.py --pin`.
4. The `libgphoto2` mentions in `SUPPORTED-DRIVERS.md` and
   `.github/instructions/gphoto.instructions.md` that name the bundled version.
