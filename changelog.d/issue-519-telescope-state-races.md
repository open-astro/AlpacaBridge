### Added (tests)
- **Telescope target concurrency coverage** (issue #519): exercise target and guide-rate accessors in telescope stress tests and verify SynScan guiding does not publish or overwrite target properties.

### Changed (docs)
- **Pi 4 ASan host limitation documented**: record that Debian's 39-bit-VA Pi kernel cannot initialize ASan's 64-bit allocator, and point pre-flight guidance to the 48-bit-VA requirement.

### Fixed
- **Telescope target and guide-rate synchronization** (AlpacaCore, issue #519): guard mutable target and guide-rate state consistently, snapshot target pairs before target operations, and keep SynScan's pulse-guide position estimate separate from public targets. SynScan async slews now seed that estimate from their destination so completion reports the new position after pulse guiding.
