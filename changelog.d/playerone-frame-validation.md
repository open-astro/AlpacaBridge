### Breaking changes
- **Player One and iCAM cameras reject malformed exposure frames** (AlpacaCore, issue #913): invalid SDK dimensions, unsupported formats, or failed downloads now surface a DriverException through ImageReady and ImageArray. StopExposure preserves partial acquired data; use AbortExposure to discard a frame. iCAM uses this same Player One camera backend. **After upgrading:** clients should handle DriverException as a failed frame and use AbortExposure when they intend to discard it.

### Fixed
- **Player One camera frame publication** (AlpacaCore, issue #913): validate supported formats, positive sensor-bounded frame dimensions, requested ROI alignment, and output shape before caching; expose acquisition failures and recover on the next valid exposure. Validate replacement requests before cancelling the current frame, preserve stop requests made during worker setup, and report completed-frame timing only after successful readout.

### Added (tests)
- **Player One fake-SDK frame regressions and stress** (issue #913): exercise malformed metadata, dirty-ROI readback refusal, aligned padding/cropping, binning, all frame formats, readout-preserving StopExposure, frame-discarding AbortExposure, early stop requests, replacement validation, readiness errors and timeouts, watchdog failure across a late frame, init/download recovery, pulse-off failure recovery, and connected lifecycle/exposure churn; iCAM routing already shares this backend.
