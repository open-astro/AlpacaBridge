### Breaking changes
- **ZWO devices reject invalid frame metadata and selectors** (AlpacaCore/AlpacaHTTP, issue #913; follows PR #1066): malformed camera frames and failed transfers now surface a DriverException instead of false readiness, and SDK device IDs/indices outside -1..INT_MAX are refused before narrowing. After upgrading: an invalid saved selector is dropped; an entry with no other valid identity stays listed but needs reconfiguration before it can connect.
- **ZWO cameras refuse geometry changes during an exposure, and a change after it discards the ready image** (AlpacaCore, issues #913, #1981): changing binning, ROI size or start position while an exposure runs returns InvalidOperation; with an image ready the change succeeds and discards that image, and the LastExposure values stay. After upgrading: read ImageArray before changing camera geometry.

### Fixed
- **ZWO camera frame publication and recovery** (AlpacaCore, issue #913): validate each exposure-time ROI/format field, preserve requested dimensions when SDK alignment shortens a full-frame ROI, keep previous exposure state when a replacement start fails, reject concurrent starts, retain last-exposure metadata across geometry writes, and clear a latched acquisition failure after AbortExposure succeeds.

### Added (tests)
- **ZWO camera regression coverage** (issue #913; follows PR #1066): cover binned full-frame alignment, abort/reconnect recovery, start failures, metadata mismatch, odd-height padding, readiness/download races, exposure retries, pixel conversion and lifecycle stress, plus API and persisted selector narrowing across repeated startups.
