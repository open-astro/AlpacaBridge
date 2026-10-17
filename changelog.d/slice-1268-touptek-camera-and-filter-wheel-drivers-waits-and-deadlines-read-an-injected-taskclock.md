### Changed

- **ToupTek camera and filter wheel waits and deadlines read an injected task clock** (ToupTek, issue #1268): the camera's pulse-guide timer, thermal poll wait, exposure deadline and thermal cache ages, and the filter wheel's homing poll sleep go through `util::TaskClock`, so tests drive them with `FakeTaskClock::advance()` instead of sleeping; production uses the real clock and behaves as before. Cases in `test_touptek_camera.cpp` and `test_touptek_filterwheel.cpp` (tag `[taskclock]`); fake-only, ConformU not re-run.
