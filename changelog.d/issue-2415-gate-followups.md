### Fixed

- **A slew's Tracking check can no longer run without its lock** (AlpacaCore Celestron NexStar, Bisque and SynScan telescope drivers, issue #2415): each driver's command-gate checks held an optional `std::unique_lock*` that only the equatorial slews set, and `check_tracking()` dereferenced it unchecked (SynScan behind an `assert` that a release build drops). The Tracking check now lives in a `SlewGateChecks` type that holds the lock by reference, and the plain `GateChecks` has no `check_tracking()`, so a slew built without the lock, or a gate family that adds a Tracking check, fails to compile. No behaviour changes.
- **ZWO: the MoveAxis stop threshold lives in the gate limits** (AlpacaCore ZWO AM telescope driver, issue #2415): `move_axis()` decided a stop with its own `|rate| < 1e-9` while the gate's limits kept a stop epsilon of 0, so the gate's `is_move_axis_stop()` disagreed with the driver. The limits now carry the largest double under 1e-9 and `move_axis()` runs `util::check_move_axis()`, so the two cannot disagree; a rate of exactly 1e-9 is still a move.

### Added (tests)

- **ZWO MoveAxis stop threshold** (issue #2415): `ZWO telescope - MoveAxis treats 1e-9 as a move and a smaller rate as a stop (#2415)` in `test_zwo_telescope_clock.cpp`.
