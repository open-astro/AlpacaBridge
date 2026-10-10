### Breaking changes
- **Player One and iCAM cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK dimensions, unsupported formats, or failed downloads now surface a DriverException through ImageReady and ImageArray. iCAM uses this same Player One camera backend.

### Fixed
- **Player One camera frame publication** (AlpacaCore, issue #913): validate format, aligned dimensions, source capacity, and output shape before caching; expose acquisition failures and recover on the next valid exposure.

### Added (tests)
- **Player One fake-SDK frame regressions and stress** (issue #913): exercise malformed metadata, undersized ROI, format mismatch, watchdog expiry with a late frame, download failure, recovery, and connected lifecycle/exposure churn; iCAM routing already shares this backend.
