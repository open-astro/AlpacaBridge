### Changed

- **Sky-Watcher telescope tests**, Sky-Watcher driver, issue #1869. `CanMoveAxis` (axes 0 and 1 true, axis 2 false, -1 and 3 `InvalidValue`) and `ApertureArea` (pi*(d/2)^2 recomputed from the diameter, 0 for a zero diameter) are now asserted on a connected driver over the fake mount in `AlpacaCore/tests/test_skywatcher_telescope.cpp`. Test only; no driver change.
