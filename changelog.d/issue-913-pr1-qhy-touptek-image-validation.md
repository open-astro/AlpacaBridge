### Breaking changes
- **QHY and ToupTek cameras reject malformed exposure frames** (AlpacaCore, issue #913): frames with invalid SDK dimensions or unsupported layouts that previously could be reported ready now surface a DriverException through ImageReady and ImageArray. **After upgrading:** if acquisition fails, inspect camera logs and verify the selected ROI and vendor SDK.
- **Player One and iCAM cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK dimensions, unsupported formats, or failed downloads now surface a DriverException through ImageReady and ImageArray. iCAM uses this same Player One camera backend.

### Fixed
- **QHY and ToupTek camera frame publication** (AlpacaCore, issue #913): validate SDK frame metadata, output shape, and available buffer capacity before reporting exposure success; the next valid exposure clears the stored failure.
- **Player One camera frame publication** (AlpacaCore, issue #913): validate format, aligned dimensions, source capacity, and output shape before caching; expose acquisition failures and recover on the next valid exposure.

### Added (tests)
- **Shared camera image shape validation and QHY/ToupTek regressions** (issue #913): cover malformed frame shapes, SDK metadata, recovery, and connected QHY exposure stress.
- **Player One fake-SDK frame regressions and stress** (issue #913): exercise malformed metadata, download failure, recovery, and connected lifecycle/exposure churn; iCAM routing already shares this backend.
