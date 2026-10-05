### Fixed
- **Telescope target and guide-rate synchronization** (AlpacaCore, issue #519): guard mutable target and guide-rate state consistently, snapshot target pairs before target operations, and keep SynScan's pulse-guide position estimate separate from public targets.

### Added (tests)
- **Telescope target concurrency coverage** (issue #519): exercise target and guide-rate accessors in telescope stress tests and verify SynScan guiding does not publish or overwrite target properties.
