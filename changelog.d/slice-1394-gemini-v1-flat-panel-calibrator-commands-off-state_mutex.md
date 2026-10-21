### Fixed

- **Gemini Flat Panel (Lite) Brightness and CalibratorState no longer wait for the serial reply**, Gemini driver, issue #1394. `CalibratorOn` and `CalibratorOff` now run their light and brightness commands under a driver I/O lock and publish the result under a short state lock afterwards, so the two getters return within 20 ms while a command is in flight and a failed command leaves the previous state in place. Covered by `Gemini Flat Panel Lite - Brightness and CalibratorState do not wait for a calibrator command` in `AlpacaCore/tests/test_gemini_flatpanel.cpp`.
