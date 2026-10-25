### Breaking changes
- **ZWO camera USB Limit now defaults to 40** (AlpacaCore, issue #1480): writable controls are set and verified in manual mode on connect rather than left at the SDK default; downloads may be slower, and a failed write/readback now refuses connect. **After upgrading:** set USB Limit in camera setup to a supported value if the conservative default is unsuitable; blank uses 40 clamped to the camera's range. Cameras without a writable control still connect when no value is configured.

### Fixed
- **Apply the missing per-camera USB bandwidth limit** (AlpacaCore, AlpacaHTTP, issue #1480): persist `usbBandwidth`, expose USB Limit in ZWO camera setup, map the SDK control in both directions, and fail cleanly when an explicit setting cannot be applied. Intended as a mitigation for marginal-link download failures, not a proven fix for the reported resets; USB reset/reopen recovery (#1500) is not included. For saved entries, see Breaking changes.
