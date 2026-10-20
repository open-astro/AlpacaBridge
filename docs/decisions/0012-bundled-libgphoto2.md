# Bundled libgphoto2

Status: accepted

## Context

The gphoto camera driver links libgphoto2. Debian trixie ships 2.5.31-4, which lacks the fixes for CVE-2026-40333, -40334, -40335, -40336, -40338, -40339, -40340 and -40341 (fixed in libgphoto2 2.5.34 per its NEWS; Debian rates them `no-dsa`), and a stable Debian release gets a new upstream version only through a point release if at all. Issue [open-astro/AlpacaBridge#933](https://github.com/open-astro/AlpacaBridge/issues/933) asked for a current library in the package, and #1137 tracked the update.

## Decision

1. **Bundle it.** The upstream 2.5.34 tarball is vendored in `AlpacaCore/external/libgphoto2/` (like libgpiod; no network fetch in any build) and `scripts/build_bundled_gphoto.sh` builds it, with the default `standard` camlib set, into the private prefix `/usr/lib/alpacabridge/libgphoto2`. `debian/rules`, CI and the shell installers run the same script. Nothing in the tree compiles against the distro's libgphoto2, so `libgphoto2-dev` is no longer a build dependency and the package no longer depends on `libgphoto2-6t64` or `libgphoto2-port12t64`.
2. **Loader path: private prefix, `DT_RPATH`, no `ld.so.conf`.** The prefix is compiled into the library, so camlibs and iolibs are found without `CAMLIBS`/`IOLIBS` in the service environment. The server and the library carry a `DT_RPATH` (linked with `--disable-new-dtags`), which the loader applies to transitive lookups, so `libgphoto2.so.6` resolves `libgphoto2_port.so.12` from the bundle even when the distro ships the same soname. The directory is never added to `/etc/ld.so.conf.d/`: that would shadow the distro library for every other program on the host. `scripts/check_bundled_gphoto.sh` checks both (the `package-deb` CI job and the nightly build job run it on the installed package; `--pid` checks a running server on the rig).
3. **udev rules come from the bundled build** (`print-camera-list udev-rules version 201 group plugdev mode 0660`, installed as `61-alpacabridge-libgphoto2.rules`), because the package no longer pulls in the distro rules file.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. Nothing in the tree compiles or links against the distro libgphoto2; the build uses the vendored tarball through `scripts/build_bundled_gphoto.sh`. Scope: `debian/control`, `debian/rules`, `scripts/build_bundled_gphoto.sh`, `AlpacaCore/external/libgphoto2/**`. Checked by: the CI `package-deb` job and the nightly build job.
2. The bundle lives in `/usr/lib/alpacabridge/libgphoto2`, is found through `DT_RPATH`, and is never registered in `/etc/ld.so.conf.d/`. Scope: `debian/rules`, `scripts/build_bundled_gphoto.sh`. Checked by: `scripts/check_bundled_gphoto.sh` (CI `package-deb` job and the nightly build job).
3. The udev rules come from the bundled build, not the distro package. Scope: `debian/rules`, `scripts/build_bundled_gphoto.sh`. Checked by: review.

## Alternatives rejected

- Wait for Debian, or publish a backport in the apt repository: neither is under our control, and a backport of the same package name would replace the host's library for every program.
- Build only the `ptp2` camlib: the standard set is what Debian ships, and the driver is documented as generic PTP, not per-model.
- Register the directory with ldconfig: it shadows the distro library host-wide.

## Consequences

- The host's own libgphoto2 stays as it is for other programs, patched or not; only AlpacaBridge gets the fixed library.
- We own updates. `scripts/dependency_updates.py` watches the libgphoto2 tags page against the SDK entry's version, so a new release opens an update issue; the bump procedure is in `AlpacaCore/external/libgphoto2/README.md`. The Debian-library baseline row for libgphoto2 is gone.
- The package grows by the library and its camlibs, and the `.deb` build compiles them (a few minutes on arm64). The release SBOM cannot match the built files to a tracked file, so `scripts/dependency_snapshot.py` skips that one subtree and still lists the tarball's SDK entry.
- The gphoto2 command-line tool is not bundled. The driver drives bulb mode through libgphoto2 itself and never runs the CLI, and no user documentation tells users to run it; bench reproduction uses a distro or locally built `gphoto2`.

## Links

- Issues [#933](https://github.com/open-astro/AlpacaBridge/issues/933), [#1137](https://github.com/open-astro/AlpacaBridge/issues/1137).
- Implementation: `AlpacaCore/external/libgphoto2/` (vendored tarball and bump procedure), `scripts/build_bundled_gphoto.sh`, `scripts/check_bundled_gphoto.sh`.
- Current rules: [GPhoto instructions](../../.github/instructions/gphoto.instructions.md).
