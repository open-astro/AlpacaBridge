### Changed

- **Sky-Watcher motion-limits tests wait on the fake clock**, AlpacaCore tests, issue #1799. The remaining cases of `test_skywatcher_motion_limits.cpp` run the driver and the fake mount on one `FakeTaskClock` and wait through `pump_until`, `follow_until` and `follow_wall_clock`, with no `sleep_for` or real-time `wait_until` left, as decision 0005 rule 2 asks. Test-only.
