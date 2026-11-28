### Fixed

- **iOptron restores Tracking after the MoveAxis dead-man and the 300 s slew cap stop**, iOptron driver, issue #2762. Like AbortSlew (#2000), a dead-man stop returns Tracking to its state before the move, and a cap stop of an equatorial GOTO ends with Tracking on; the tracking command goes out once the stop check has verified the stop. Covered by the `[goto-safety]` "iOptron motion bound - ... leaves Tracking ..." cases in `test_ioptron_telescope.cpp`.
