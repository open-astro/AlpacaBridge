### Changed

- **CI pins zizmor 1.31.1** (CI tooling, issue #1370): the `zizmor` job in `.github/workflows/ci.yml` and `scripts/ci_preflight.sh` now install v1.31.1 (the `aarch64-unknown-linux-gnu` tarball, checked against its SHA-256); `zizmor --offline .github/workflows/` reports no findings with it.
