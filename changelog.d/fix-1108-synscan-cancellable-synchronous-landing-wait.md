### Fixed

- **SynScan AbortSlew during the final approach of a synchronous slew**, SynScan driver, issue #1108. `AbortSlew`, `MoveAxis`, a new slew, park or disconnect during the timed landing move of a `SlewToCoordinates` or `SlewToTarget` now returns the waiting caller at once instead of after the move's remaining time (up to 8 s). Covered by `AlpacaCore/tests/test_synscan_goto_landing.cpp` (`AbortSlew during a synchronous slew's landing nudge`).
