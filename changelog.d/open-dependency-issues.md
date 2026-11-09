### Changed
- **libgpiod updated to 2.3.2** (CI and nightly builds), issue #2200: the vendored tarball moves from 2.2.4, and because 2.3 replaces autotools with meson, the jobs that build it install meson and build with it. The `.deb` still takes libgpiod from Debian.
- **CI tools updated** (CI), issues #2202, #2201 and #2199: zizmor 1.32.0, PMD 7.29.0 and OSV-Scanner 2.7.1.
- **Vendored SDK folders no longer carry a version** (AlpacaCore, packaging), issue #1305: the ToupTek, Altair, QHY and both Player One folders have fixed names, and `dependency_snapshot.py --check` compares each SDK's `version` with the version its vendored files state.
- **INDI review baselines advanced** (tooling), issues #2198 and #2197: the AM5 and OnStep driver changes add no protocol the AlpacaBridge drivers depend on.

### Fixed
- **WandererRotator: a move refused by the firmware ends at once with a named fault** (AlpacaCore), issue #1960: a bare `NP` or `LIMIT` reply now makes `IsMoving` report no motor power or a position limit instead of waiting out the budget and blaming a missing completion report. Firmware older than 20261025 draws a warning at connect.

### Added (tests)
- **WandererRotator NP and LIMIT replies** (issue #1960): one test case over both replies.
