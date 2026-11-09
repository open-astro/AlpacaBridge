### Fixed

- **LastExposureDuration and LastExposureStartTime before any image no longer log at ERROR**, camera drivers and the iOptron iGuider camera, issue #2182. The client still gets InvalidOperation (0x40B, HTTP 200) as ICameraV4 requires; the server log line is now DEBUG (`case_run_last_exposure_early_read_logs_debug` in `AlpacaHTTP/tests/test_device_call.cpp`; `require_last_exposure_unset` asserts `is_asked_too_early()` on both getters).
