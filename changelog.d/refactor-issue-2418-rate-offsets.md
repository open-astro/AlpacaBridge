### Changed
- **Telescope command gate: the rate offsets join it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_rate_offset` for `RightAscensionRate` and `DeclinationRate` writes (the value, then connected, the link fault and TrackingRate sidereal) with its argument and state halves. Sky-Watcher and OnStep, the two mounts that implement the writes, use the argument half and keep their own state checks, which run one more check inside that order (the site on Sky-Watcher, the firmware on OnStep). No error code, message or order changes for clients.

### Added (tests)
- **Telescope command gate rate-offset rows** (issue #2418): the value scaled to the mount's unit against its limit, each axis's text, the order connected, fault then TrackingRate, and no parked or Tracking check.
