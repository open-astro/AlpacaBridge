### Fixed

- **Sky-Watcher synchronous goto and pulse window**, Sky-Watcher driver, issue #2066. `SlewToCoordinates` and `SlewToTarget` now supersede an async slew body in flight instead of waiting for it to finish, so a second motion command no longer stalls behind the first (decision 0006). A pulse whose dispatch fails, or that AbortSlew or any other reaper ends, no longer leaves its ownership window armed (decision 0022). Covered by the three `(#2066)` cases in `AlpacaCore/tests/test_skywatcher_async.cpp`.
