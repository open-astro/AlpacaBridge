### Breaking changes
- **ZWO AM UTC anchors no longer survive a connection lifetime** (AlpacaCore, issue #1522): a fresh connect, disconnect, failed initialization or lost link clears the previous UTC anchor. With connect-time synchronization disabled and no successful mount-time sample, readback no longer extrapolates a previous session's client time. **After upgrading:** clients relying on that cached value must write UTCDate after connecting, or enable connect-time host synchronization. Offline writes remain available until the next lifecycle boundary; redundant Connected=true retains the existing refresh behavior.

### Fixed
- **ZWO AM UTC cache adopts the shared UtcOffset** (AlpacaCore, issue #1522): preserve fractional monotonic elapsed time, client writes, host synchronization and periodic mount-time refresh while preventing an old session from republishing its UTC anchor; for reconnect readback changes, see Breaking changes.

### Changed (tests)
- **ZWO AM timer regressions use virtual time** (AlpacaCore, issue #1522): replace real waits for slew, tracking-cache and pulse deadlines with FakeTaskClock rendezvous, retaining deliberate transport latency and bounded concurrency observation.
- **ZWO AM UTC loss and GOTO cancellation coverage**: exercise TCP removal and aged telemetry faults, UTC refusal while lost, explicit reconnect without a stale anchor, and disconnect during the pre-GOTO time write. A lost link retains `NotConnected`; host fallback applies after deliberate disconnect or a fresh unsynchronized connection, not while lost.

### Changed (docs)
- **AM5 clock-adoption qualification refreshed** (issue #1522): publish privacy-reviewed USB and native handset Wi-Fi ConformU 4.5.2 reports from the same OPi6 arm64 binary on mount firmware 1.8.8; each preserves all 422 rows and 103 timing measurements with no errors, warnings, issues or timing violations.
