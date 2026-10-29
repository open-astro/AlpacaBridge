### Breaking changes
- **ZWO AM site-coordinate reads require a healthy connection** (AlpacaCore, issue #1681): SiteLatitude and SiteLongitude now raise DriverException while the link is faulted and NotConnected while disconnected or lost instead of returning cached coordinates. **After upgrading:** connect before reading site coordinates and handle communication errors; offline site configuration writes remain available.

### Fixed
- **ZWO AM5 Park completion honors the explicit mount reply** (AlpacaCore, issue #1681 validation): a completed Park no longer waits forever when the general status omits its stopped flag; AtPark and Slewing agree after the existing startup guard and accepted telemetry sample.
- **ZWO AM refuses faulted site-coordinate caches** (AlpacaCore, issue #1681): both site getters use the existing link-health and loss checks without device I/O; for changed read behavior, see Breaking changes.

### Added (tests)
- **ZWO GOTO tests wait for setup publication** (AlpacaCore, issue #1681 validation): retarget and position-feedback tests wait for polling to resume before advancing the fake clock past arrival, avoiding scheduler-dependent sanitizer failures.
- **ZWO AM commanded-state fault coverage** (AlpacaCore, issue #1681): fake-mount regressions cover pulse and rate reads, an active guide overlapping an in-flight failed poll, site recovery, motion shortcuts, safety stops, and getter-triggered loss at the 30-second deadline with held I/O.
