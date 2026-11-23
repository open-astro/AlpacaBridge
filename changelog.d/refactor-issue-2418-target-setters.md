### Changed
- **Telescope command gate: the target setters join it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_target_right_ascension` and `check_target_declination` (the value, then connected and the link fault) with their argument halves and a shared `check_target_state`. All eight mount drivers use the argument halves; SynScan, iOptron and Bisque also use the state half, and the other five keep their own state checks, which differ on purpose (NexStar, OnStep and ZWO AM store a target while disconnected). No error code, message or order changes for clients.

### Added (tests)
- **Telescope command gate target-setter rows** (issue #2418): each value in `util::validate_ra()` or `util::validate_dec()`'s words, the order connected then fault, and no parked or Tracking check.
