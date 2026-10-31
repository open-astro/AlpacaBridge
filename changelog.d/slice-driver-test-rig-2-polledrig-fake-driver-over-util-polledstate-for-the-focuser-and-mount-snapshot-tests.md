### Changed

- **Driver test rig `PolledRig`**, tests only, issue #1905. `AlpacaCore/tests/driver_rig.h` gains `PolledRig<Fake, Driver>`, which owns a fake, a `FakeTaskClock` and the driver and wraps `connect_and_park`, `tick` and `require_alpaca_error`; the QHY, Gemini, Astroasis and Celestron focuser tests use it in place of their per-file setup. Covered by the `PolledRig` cases in `AlpacaCore/tests/test_driver_rig.cpp`.
