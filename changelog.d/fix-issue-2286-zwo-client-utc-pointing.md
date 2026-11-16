### Fixed

- **ZWO AM: SiderealTime, pier side and the pre-GOTO time write follow a client's UTCDate** (AlpacaCore, issue #2286): after a client wrote `UTCDate`, `SiderealTime`, `SideOfPier` and `DestinationSideOfPier` still used the host clock, and the next GOTO (or a `Connected=true` refresh with time sync on) wrote the host clock back to the mount; the client's time now stays the one time source until the next connect or disconnect. When the driver writes the host clock to the mount and the kernel reports it unsynchronized, it logs one warning per connection.

### Added (tests)

- **ZWO client UTCDate pointing cases** (issue #2286): SiderealTime, the pre-GOTO `:SMTI`, pier side and the unsynchronized-host warning; the "seeds UTC at connect refresh and before GOTO" case now expects the client's time to survive the refresh and the GOTO.
