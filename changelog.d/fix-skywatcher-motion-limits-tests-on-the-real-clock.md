### Changed

- **Sky-Watcher motion-limits tests wait on the fake clock**, AlpacaCore tests, issue #1799. 26 more cases of `test_skywatcher_motion_limits.cpp` now run the driver and the fake mount on one `FakeTaskClock` and wait through `pump_until`, `follow_until` and `follow_wall_clock`; no case in the file uses `sleep_for` or a real-time `wait_until` any more, as decision 0005 rule 2 asks. Test-only.
