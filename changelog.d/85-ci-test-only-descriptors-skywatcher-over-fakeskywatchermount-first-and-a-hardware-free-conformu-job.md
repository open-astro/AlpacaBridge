### Added

- **Hardware-free ConformU job for the Sky-Watcher driver**, CI and tests, issue #1614. The `conformu-fake` job builds the test-only `fake_skywatcher_server` (a test catalog descriptor over `FakeSkyWatcherMount`, not installed and not linked into `alpacabridge`), runs ConformU 4.5.3 against its telescope and uploads the log. `scripts/check_conformu_reports.py` now requires a report under a `Fake*` path to be a `.txt` whose first line starts `FAKE REPORT:`, so a fake run is never read as hardware validation (self-test cases in the script). ConformU was not re-run on hardware.
