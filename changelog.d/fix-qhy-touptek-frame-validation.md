### Breaking changes
- **QHY and ToupTek cameras reject malformed exposure frames** (AlpacaCore, issue #913): frames with non-positive/invalid dimensions, unsupported layouts, or insufficient buffers surface a DriverException through ImageReady and ImageArray; positive short QHY frames retain the requested ROI shape and are zero-padded, while ToupTek frames smaller than the size the ROI predicts now fail with DriverException instead of being zero-padded (an odd NumX/NumY at bin 2 and above is handled separately, see issue #977). **After upgrading:** if acquisition fails, inspect camera logs and verify the selected ROI and vendor SDK.

### Fixed
- **QHY and ToupTek camera frame publication** (AlpacaCore, issue #913): validate SDK frame metadata and buffer capacity before reporting exposure success; abort and reconnect discard stale exposure state.
- **QHY stress-registration guidance** (developer docs): update `/driver-build` to identify the registered fake-SDK camera stress case.

### Added (tests)
- **Shared camera image shape validation and QHY/ToupTek regressions** (issue #913): cover malformed-frame recovery after reconnect, no-frame and late-frame failures, oversized ToupTek dimensions, aborted waits, QHY short-frame padding, SDK metadata, and connected QHY exposure stress.
