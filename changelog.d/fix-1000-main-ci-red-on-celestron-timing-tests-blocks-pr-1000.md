### Fixed

- **Celestron async slew-failure tests no longer fail for about half of every UTC day** (Celestron, issue #1005): the cases slewed to a fixed RA of 5.5 h, which takes a forced meridian flip (two passthrough GOTOs, no plain one) whenever the hour angle is negative against the fake mount's pier W at longitude 0; `no_flip_ra_hours()` in `test_celestron_async_slew_failure.cpp` now picks the target one hour west of the local sidereal time; test-only, no production change.
