### Breaking changes
- **SVBONY cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK ROI metadata, unsupported formats, or incomplete frames that previously could be published as ready now surface a DriverException through ImageReady and ImageArray.

### Fixed
- **SVBONY camera frame publication** (AlpacaCore, issue #913): verify SDK ROI/format readbacks and source capacity before caching; expose conversion/acquisition failures and recover on the next valid exposure.

### Added (tests)
- **SVBONY fake-SDK frame regressions and stress** (issue #913): exercise valid publication, malformed ROI/format rejection, failure reporting, recovery, and connected lifecycle/exposure churn.
