### Changed
- **Telescope command gate: the Alt/Az slews and SyncToAltAz join it** (AlpacaCore), issue #2418. `util/telescope_command_gate.h` gains `check_alt_az_slew` for `SlewToAltAz` and `SlewToAltAzAsync` (altitude and azimuth, then connected, parked and fault in the driver's order, the site, then Tracking on) and `check_alt_az_sync` for `SyncToAltAz` (the same, with no Tracking check), each with its argument and state halves. OnStep and iOptron, the two mounts that implement them, use them for every member they implement; iOptron's limits put the site check before the parked check, as before. No error code, message or order changes for clients.

### Added (tests)
- **Telescope command gate Alt/Az rows** (issue #2418): the altitude and azimuth ranges and texts, azimuth 360 per driver, the member-named texts, the order connected, parked, fault, site and Tracking on, the site check first, and no Tracking check for SyncToAltAz.
