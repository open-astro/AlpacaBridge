### Fixed

- **SynScan: a saved-hour-angle Park no longer fails with "RA out of range"** (issue #869). The park RA is now wrapped into [0, 24) by `park_ra_from_hour_angle()`, so a tiny negative remainder that rounded to exactly 24.0 is brought back to 0 (covered by the "Park RA wraps below 24 h" case in `test_synscan_telescope.cpp`).
