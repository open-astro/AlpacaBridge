### Changed
- **Telescope command gate: SyncToCoordinates joins it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_sync` (RA and Dec, then connected, then parked and the link fault in the driver's order, with no Tracking check) with its argument and state halves, and a `sync_order` limit. All eight mounts use the argument half; NexStar, SynScan, iOptron, ZWO AM, Bisque and OnStep also use the state half, and Sky-Watcher and Celestron CGX keep their own state checks, which run a site check inside that order. The sync bound stays each driver's, after the gate. No error code, message or order changes for clients.

### Added (tests)
- **Telescope command gate sync rows** (issue #2418): the RA and Dec texts, each parked-against-fault order (parked first, fault first, no fault check), and no Tracking check.
