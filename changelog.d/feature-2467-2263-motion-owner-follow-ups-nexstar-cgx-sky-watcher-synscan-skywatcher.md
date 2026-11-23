### Fixed

- **Sky-Watcher superseded pulse dispatch logged at INFO**, Sky-Watcher driver, issue #2467. A PulseGuide whose dispatch loses its axis to a newer motion command is an expected outcome, so it is logged at INFO instead of as a WARN dispatch failure. Tests also show that a MoveAxis on the other axis does not supersede the per-axis stop-waits of Tracking false, DeclinationRate (zero and duty rates) and an RA duty rate (`test_skywatcher_motion_owner.cpp`).
