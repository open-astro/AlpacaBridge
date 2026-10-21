### Fixed (diagnostics)
- **QHY scan failures no longer disappear from the logs** (AlpacaCore, issue #1211): report raw scan/init results and timing, failed ID lookups, and usable-ID counts without changing connection or SDK lifecycle behavior; no automatic SDK reset is introduced.

### Fixed
- **QHY reconnect after a power cycle** (AlpacaCore, issue #1211): isolate SVBONY's bundled USB symbols so the shared QHY SDK uses system libusb. Matched stable-based arm64 rig tests with QHY SDK 26.06.04.16 recovered the integrated wheel and shared camera/wheel without a service restart or SDK reset; the all-vendors server also recovered, captured a fresh image, moved the wheel, and passed camera and wheel ConformU with all timing targets met. Supplemental reports retain that build's provenance; main's newer SDK is not covered by those runs. Stale USB descriptors still accumulate.

### Added (tooling)
- **Manual QHY USB recovery probe** (issue #1211): an acknowledged arm64-only SDK experiment with raw-open and driver-matching wheel setup, guarded held handles, execution-context records, fake-only tests, and a documented rig checklist for testing in-process SDK reinitialization; no automatic daemon reset is enabled.
