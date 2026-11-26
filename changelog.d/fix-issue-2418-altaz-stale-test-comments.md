### Changed

- **OnStep Alt/Az test comments name the command gate**, OnStep test suite, issue #2418. Three comments in `test_onstep_telescope.cpp` and `test_onstep_telescope_snapshot.cpp` named the OnStep Alt/Az helpers that #2618 replaced with `util::check_alt_az_slew()`, `util::check_alt_az_sync()` and `util::check_alt_az_arguments()`; they now name the gate, and their "Falsified by" mutations apply on the current code. Comments only: no assertion changes. The fix was pushed to #2618 after it was queued, and the queue merged the approved head without it.
