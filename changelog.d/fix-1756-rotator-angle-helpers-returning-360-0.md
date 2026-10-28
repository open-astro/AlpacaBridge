### Fixed

- **Rotator angle wrap**, ZWO CAA and WandererAstro rotator drivers, issue #1756. Position, MechanicalPosition and the target no longer round up to exactly 360 (or `max_degree` on a limited ZWO rotator) when the angle is a hair below zero; the result is always in `[0, wrap)`. Covered by `A tiny negative target stays below the wrap` in `AlpacaCore/tests/test_zwo_rotator.cpp` and `A tiny negative target stays below 360` in `AlpacaCore/tests/test_wandererastro_rotator.cpp`.
