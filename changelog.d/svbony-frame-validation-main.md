### Breaking changes
- **SVBONY cameras reject invalid frame metadata and unsupported output formats** (AlpacaCore, issue #913): invalid SDK ROI/format readbacks, unsupported formats, or insufficient frame storage now fail instead of publishing an invalid image; cameras with no supported connect-time format refuse connection. `CanStopExposure` is now false because the SDK does not guarantee partial-frame readout, and `SVBGetVideoData` does not report bytes written. **After upgrading:** use AbortExposure to discard an exposure; retry an abort stop failure or disconnect and reconnect before reuse; if connection is refused, verify that the model is supported by the installed SVBONY SDK.

### Fixed
- **SVBONY camera frame publication** (AlpacaCore, issue #913): validate SDK ROI/format readbacks and required storage, keep image conversion outside the driver state lock, surface acquisition failures through ImageReady and ImageArray, and allow a later valid exposure to recover.

### Added (tests)
- **SVBONY fake-SDK acquisition regressions** (issue #913): cover supported formats, finite read waits, ROI boundaries, cancellation and destruction, watchdog latching, metadata mismatches, fatal versus transient read results, repeated stop failure and recovery, write exclusion, and recovery through a scripted SDK.
