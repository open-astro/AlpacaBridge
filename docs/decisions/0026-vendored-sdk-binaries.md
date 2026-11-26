# Vendored SDK binaries

Status: accepted

## Context

Six vendors (ZWO, with its EAF, EFW and CAA accessories, QHY, SVBONY, ToupTek, Altair and Player One) are driven through closed SDK libraries that exist only as prebuilt binaries. A driver cannot be built or run without them.

`AlpacaCore/.gitignore` ignores SDK files by default and allowlists one subtree per vendor (`!external/ZWO/ASI_Camera_SDK/**` and the like). At this record's date the allowlisted subsets are 220 tracked files under `AlpacaCore/external/`, 22 of them shared libraries, all arm64.

Three of those subsets (QHY, ToupTek, SVBONY) carry no written redistribution grant from the vendor. ZWO and Altair ship an MIT `license.txt`, and Player One's license allows use without restriction with the notice kept (`AlpacaCore/external/README.md`, "Vendored SDK provenance and license status"). The repository is AGPL-3.0-or-later, and the drivers link closed libraries.

Other programs on the same board load the camera libraries too. SmartGuider opens `libASICamera2.so`, `libqhyccd.so` and `libtoupcam.so` from the system library path.

## Decision

Four parts.

- **The SDK subset each driver needs is committed to this repository.** Headers and arm64 libraries, unmodified, as the vendor's public download ships them. A clean clone builds every driver with no download step and no network fetch.
- **The `.deb` redistributes the shared libraries on the system linker path.** `debian/rules` copies each `.so` into `/usr/lib/alpacabridge/` and writes `/etc/ld.so.conf.d/alpacabridge.conf`; the `postinst` runs `ldconfig`. Every camera vendor ships its `.so` this way even when the server links the SDK statically, because companion programs load it by name.
- **The license carries a linking exception.** `LICENSE` ends with an additional permission that allows combining AlpacaBridge with a Vendor SDK and conveying the result. It covers AlpacaBridge's side only; each SDK keeps its own terms.
- **No vendor is asked for written permission.** Decided in issue #1758: the QHY, ToupTek and SVBONY subsets stay, unmodified, on the basis that they come from the vendors' public SDK pages. If a vendor objects, its subset is removed and that vendor's driver goes back to a download-the-SDK-yourself build.

Every vendored subset is pinned: `scripts/dependency_sdk_versions.json` holds its version and a sha256 over its tracked files.

libgphoto2 is the opposite case and has its own record ([decision 0012](0012-bundled-libgphoto2.md)): open source, built from a vendored tarball into a private prefix, never registered with ldconfig.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A vendor SDK subset is committed only under its vendor's directory in `AlpacaCore/external/`, with an allowlist line for that directory in the ignore file. Scope: `AlpacaCore/.gitignore`, `AlpacaCore/external/**`. Checked by: review (`git check-ignore -v` on a new library shows the vendor's `!external/<Vendor>/**` line winning).
2. Vendored SDK files are committed as the vendor ships them; no file under a vendor subset is patched, stripped or rebuilt. Scope: `AlpacaCore/external/**`. Checked by: `scripts/dependency_snapshot.py --check` (a subset that changed without a new sha256 pin fails; CI job `static-checks`), and review (a re-pinned subset names the vendor download and version it came from).
3. Every vendored subset has an entry in the SDK table with its path, version and sha256, and a row in the provenance table that names its written license or says it has none. Scope: `scripts/dependency_sdk_versions.json`, `AlpacaCore/external/README.md`. Checked by: `scripts/dependency_snapshot.py --check`, and review (the provenance row of a new or bumped subset).
4. Only arm64 SDK libraries are committed. Scope: `AlpacaCore/external/**`. Checked by: review (no `x86`, `x64` or `amd64` library path in the diff).
5. Every camera vendor's shared library is copied into `/usr/lib/alpacabridge/` by the package build, and that directory is registered with the system linker. Scope: `debian/rules`, `debian/alpacabridge.postinst`. Checked by: review (a `cp -a` line for the new library in `override_dh_auto_install`, and the `ld.so.conf.d` and `ldconfig` lines still present).
6. The Vendor SDK additional permission stays in the license text, word for word. Scope: `LICENSE`. Checked by: review (the final section of `LICENSE` is unchanged).

## Alternatives rejected

- **Download-the-SDK-yourself builds.** The contributor or the CI job fetches each SDK from the vendor, so a clean clone no longer builds. `AlpacaCore/external/README.md` gives turnkey builds as the reason for vendoring. This is the fallback if a vendor objects.
- **Ask each vendor for written permission first.** Considered and declined in issue #1758. The issue records the choice and no reason beyond it.
- **A private library directory found through `DT_RPATH`.** What decision 0012 does for libgphoto2. It would hide the camera libraries from SmartGuider and any other program that loads them by name.
- **Link every SDK statically and ship no `.so`.** Same loss for companion programs. The packaging instructions require the `.so` in the package whether or not the server links the SDK statically.

## Consequences

- The repository redistributes three SDK subsets without a written grant. The exposure is a takedown request, and the answer to one is already decided: remove the subset.
- A security fix inside a vendor library reaches users only when the vendor ships a new SDK and it is bumped here. `libtoupcam.so` and `libaltaircam.so` each carry a private, statically linked libusb of unknown version that `apt upgrade` never patches (`AlpacaCore/external/README.md`).
- `/usr/lib/alpacabridge` is on the host-wide linker path, so a library there can shadow a same-named library from another package.
- CodeQL does not scan `AlpacaCore/external/**`.
- An SDK bump is a binary diff no reviewer can read. The sha256 pin shows that the files changed and nothing about what changed.
- Every clone and every CI checkout carries the tracked files under `AlpacaCore/external/`, about 105 MB at this record's date, and git history keeps every past version of them.

## How to reverse

For one vendor: delete its subtree and its allowlist line, remove its `cp -a` line from `debian/rules` and its entry from `scripts/dependency_sdk_versions.json`, and make its `CMakeLists.txt` fail with a message that names the vendor's download page. The driver then builds only where someone placed the SDK by hand, and the package cannot ship that vendor until its `.so` comes from somewhere else. For all vendors the same steps apply six times, plus a CI step that fetches the SDKs or a build that skips those drivers. The files stay in git history either way; removing them from history is a rewrite of every branch.

## Links

- Issue [#1758](https://github.com/open-astro/AlpacaBridge/issues/1758): the decision not to ask for written permission, and the libusb finding.
- Provenance and license status per vendor: [AlpacaCore/external/README.md](../../AlpacaCore/external/README.md).
- How-to checklist for a new SDK: [packaging and release instructions](../../.github/instructions/packaging-release.instructions.md#vendor-sdk-shared-library-packaging).
- [Bundled libgphoto2](0012-bundled-libgphoto2.md), [SDK seams](0015-sdk-seams.md).
