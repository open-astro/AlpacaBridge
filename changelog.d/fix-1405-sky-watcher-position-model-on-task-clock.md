### Changed

- **Sky-Watcher dead-reckoned position on the task clock**, Sky-Watcher driver, issue #1405. The position model (the elapsed time that extrapolates RA and Dec from the commanded rates, and the time stamp of each sampled frame) now reads the injected task clock, as the snapshot frame age already did, so a fake clock's `advance()` moves the model. The UTC anchor and the host-discipline resample still read the host clock. Covered by the "the dead-reckoned position follows the task clock, not wall time" case in `AlpacaCore/tests/test_skywatcher_snapshot.cpp`.
