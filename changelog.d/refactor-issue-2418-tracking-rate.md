### Changed
- **Telescope command gate: the TrackingRate write joins it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_tracking_rate` (the rate from 0 to the driver's highest, then connected and the link fault) with its argument and state halves. The Sky-Watcher, Celestron CGX, iOptron, OnStep, Bisque and ZWO AM telescopes use it; NexStar and SynScan do not implement the write. No error code, message, limit or order changes for clients.

### Added (tests)
- **Telescope command gate TrackingRate rows** (issue #2418): the rate range and the driver's text, the order connected then fault, no parked or Tracking check, and the no-limit default.
