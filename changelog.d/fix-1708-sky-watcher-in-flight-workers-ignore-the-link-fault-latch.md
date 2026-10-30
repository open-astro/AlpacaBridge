### Fixed

- **Sky-Watcher background workers on a faulted link**, Sky-Watcher driver, issue #1708. A rate-applied re-kick (`:I`/`:J`), a Park or FindHome body started before the fault, and the end of a guide pulse no longer write to a link that is faulted or to a board that restarted; the pulse end still stops the axis and skips the tracking-drive restore. Covered by the three `linkhealth` cases in `AlpacaCore/tests/test_skywatcher_serial.cpp` that restart the board and assert no `:I`, `:J`, `:G`, `:K` or `:S` frame follows.
