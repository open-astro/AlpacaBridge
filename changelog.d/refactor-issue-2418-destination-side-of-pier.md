### Changed
- **Telescope command gate: DestinationSideOfPier joins it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_destination_side_of_pier` (RA and Dec, then connected and the link fault) with its argument and state halves. All eight mount drivers use the argument half; NexStar, SynScan and ZWO AM also use the state half, and the other five keep their own state checks, which differ on purpose. No error code, message or order changes for clients.

### Added (tests)
- **Telescope command gate DestinationSideOfPier rows** (issue #2418): RA before Dec in `util::validate_ra_dec()`'s words, the order connected then fault, and no parked or Tracking check.
