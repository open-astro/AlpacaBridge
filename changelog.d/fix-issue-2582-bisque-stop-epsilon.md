### Fixed

- **Bisque: the MoveAxis stop threshold lives in the gate limits** (AlpacaCore Bisque telescope driver, issue #2582): `move_axis()` decided a stop with its own `|rate| < 1e-9` while the gate's limits kept a stop epsilon of 0, so the gate's `is_move_axis_stop()` disagreed with the driver. The limits now carry the largest double under 1e-9 and `move_axis()` asks `util::is_move_axis_stop()`, so the two cannot disagree; a rate of exactly 1e-9 is still a move.

### Added (tests)

- **Bisque MoveAxis stop threshold** (issue #2582): `Bisque MoveAxis - 1e-9 is a move and a smaller rate is a stop (#2582)` in `test_bisque_telescope.cpp`.
