### Changed (tests)

- **QHY camera stress case documents why DriverException is not an expected error** (AlpacaCore tests, issue #980): a comment in `AlpacaCore/tests/qhy_camera_concurrency_stress.cpp` records that a racing disconnect cancels and joins the exposure worker before closing the handle and that the watchdog margin cannot elapse, so the guard's expected set stays `{NotConnected, InvalidOperation}`.
