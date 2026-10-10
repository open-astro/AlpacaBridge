### Breaking changes
- **ZWO cameras reject invalid ROI readbacks and lazy-download failures** (AlpacaCore, issue #913): unsupported SDK formats and failed or inconsistent frame downloads now surface a DriverException through ImageReady and ImageArray instead of leaving the exposure ready with stale or fabricated pixels.

### Fixed
- **ZWO camera frame publication** (AlpacaCore, issue #913): validate exposure-time ROI/format metadata and output shape, preserve padded/cropped geometry, and latch lazy-transfer failures until a new exposure succeeds.

### Added (tests)
- **ZWO fake-SDK frame regressions and stress** (issue #913): cover lazy readiness, padded Raw16/RGB24 conversion, metadata mismatch, transfer failure/recovery, duplicate reads, abort/disconnect during download, and connected lifecycle/exposure churn.
