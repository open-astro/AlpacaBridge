### Fixed

- **Sky-Watcher: the MoveAxis stop threshold lives in the gate limits** (AlpacaCore Sky-Watcher telescope driver, issue #2605): `move_axis()` decided a stop with its own `kStopEpsilon = 1e-9` and `<=` while the gate's limits kept a stop epsilon of 0, so the gate's `is_move_axis_stop()` disagreed with the driver. The limits now carry exactly 1e-9 and `move_axis()` asks `util::is_move_axis_stop()`, so the two cannot disagree; a rate of exactly 1e-9 is still a stop.

### Added (tests)

- **Sky-Watcher MoveAxis stop threshold** (issue #2605): `SkyWatcher async - MoveAxis 1e-9 is a stop and the next double up is a move (#2605)` in `test_skywatcher_async.cpp`.
