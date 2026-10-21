### Added (tests)

- **Reproduce the serial port retune and the stuck rotator Disconnect on a pty pair** (WandererAstro rotator, Gemini focuser, issue #1202). Two `[!shouldfail]` cases in `AlpacaCore/tests/test_serial_port_retune_repro.cpp` show that a Gemini focuser connect opens a port a connected rotator holds and changes its baud and `VTIME`, and that a `VMIN=1` retune keeps `WandererRotatorProtocolWrapper::disconnect()` from returning within 3 s. No production change; later slices fix the defect and remove the tags.
