### Breaking changes
- **QHY and ToupTek cameras reject malformed exposure frames** (AlpacaCore, issue #913): frames with invalid SDK dimensions or unsupported layouts that previously could be reported ready now surface a DriverException through ImageReady and ImageArray. **After upgrading:** if acquisition fails, inspect camera logs and verify the selected ROI and vendor SDK.
- **Player One and iCAM cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK dimensions, unsupported formats, or failed downloads now surface a DriverException through ImageReady and ImageArray. iCAM uses this same Player One camera backend.
- **SVBONY cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK ROI metadata, unsupported formats, or incomplete frames that previously could be published as ready now surface a DriverException through ImageReady and ImageArray.
- **ZWO cameras reject invalid ROI readbacks and lazy-download failures** (AlpacaCore, issue #913): unsupported SDK formats and failed or inconsistent frame downloads now surface a DriverException through ImageReady and ImageArray instead of leaving the exposure ready with stale or fabricated pixels.

### Fixed
- **QHY and ToupTek camera frame publication** (AlpacaCore, issue #913): validate SDK frame metadata, output shape, and available buffer capacity before reporting exposure success; the next valid exposure clears the stored failure.
- **Player One camera frame publication** (AlpacaCore, issue #913): validate format, aligned dimensions, source capacity, and output shape before caching; expose acquisition failures and recover on the next valid exposure.
- **SVBONY camera frame publication** (AlpacaCore, issue #913): verify SDK ROI/format readbacks and source capacity before caching; expose conversion/acquisition failures and recover on the next valid exposure.
- **ZWO camera frame publication** (AlpacaCore, issue #913): validate exposure-time ROI/format metadata and output shape, preserve padded/cropped geometry, and latch lazy-transfer failures until a new exposure succeeds.

### Added (tests)
- **Shared camera image shape validation and QHY/ToupTek regressions** (issue #913): cover malformed frame shapes, SDK metadata, recovery, and connected QHY exposure stress.
- **Player One fake-SDK frame regressions and stress** (issue #913): exercise malformed metadata, download failure, recovery, and connected lifecycle/exposure churn; iCAM routing already shares this backend.
- **SVBONY fake-SDK frame regressions and stress** (issue #913): exercise valid publication, malformed ROI/format rejection, failure reporting, recovery, and connected lifecycle/exposure churn.
- **ZWO fake-SDK frame regressions and stress** (issue #913): cover lazy readiness, padded Raw16/RGB24 conversion, metadata mismatch, transfer failure/recovery, duplicate reads, abort/disconnect during download, and connected lifecycle/exposure churn.
