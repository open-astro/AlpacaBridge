# libgpiod 2.2.4 source tarball (vendored build dependency)

`libgpiod-2.2.4.tar.xz` — pristine upstream release tarball, originally
published at `https://mirrors.edge.kernel.org/pub/software/libs/libgpiod/`.

sha256: `13207176b0eb9b3e0f02552d5f49f5a6a449343ce47416158bb484d9d3019592`
(matches kernel.org's published sha256sums for the release).

**Why vendored:** CI builds libgpiod v2 from source (the ubuntu-24.04 runner's
apt ships 1.6; the ZWO switch/GPIO drivers need >= 2.0). On 2026-07-01
kernel.org removed old libgpiod point releases from its distribution tree and
every mirror synced the deletion within hours, 404-ing CI twice in one day —
first the primary, then the whole mirror-fallback chain. This copy was
retrieved from the Fedora lookaside cache
(`src.fedoraproject.org/repo/pkgs/libgpiod/`) and verified byte-identical to
the original kernel.org artifact via the pinned sha256 above.

**What the CI checksum does and does not protect:** with the tarball and the
pin living in the same repo, the `sha256sum -c` step guards against
*corruption* (a bad checkout, filter mangling, bit-rot) — NOT against
adversarial modification, since one commit can swap both the binary and the
pin together. That is the standard vendoring trade-off; review changes to
this directory with the same care as a dependency bump (the provenance chain
above is what a reviewer should re-verify).

Replace on a version bump: update the tarball, this README (checksum +
provenance), and the `LIBGPIOD_VER` / `LIBGPIOD_SHA256` values in the single
top-level `env:` block of `.github/workflows/ci.yml`.
