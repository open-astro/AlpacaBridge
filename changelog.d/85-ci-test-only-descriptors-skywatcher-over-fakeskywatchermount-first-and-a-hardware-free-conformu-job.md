### Added

- **Hardware-free ConformU job for the Sky-Watcher driver**, CI and tests, issue #1614. The `conformu-fake` job builds the test-only `fake_skywatcher_server` (a test catalog descriptor over `FakeSkyWatcherMount`, not installed and not linked into `alpacabridge`), runs ConformU 4.5.3 against its telescope and uploads the log. `scripts/check_conformu_reports.py` now requires a report under a `Fake*` path to be a `.txt` whose first line starts `FAKE REPORT:`, so a fake run is never read as hardware validation (self-test cases in the script). ConformU was not re-run on hardware.

### Fixed

- **Sky-Watcher: Declination read at the end of a Dec PulseGuide**, issue #1614. The pulse stop zeroed the Dec dead-reckoning rate without folding the motion since the last anchor into the model, so the first Declination read after `IsPulseGuiding` cleared was short by up to the position cache TTL of motion (ConformU: "Declination change +00:00:30.3, Expected +00:00:37.6") until the next status poll. Test: "SkyWatcher async - Declination read right after a Dec pulse already holds the pulse's full motion".
