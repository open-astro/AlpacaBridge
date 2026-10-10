### Fixed

- **SynScan AbortSlew no longer leaves `Slewing` true when the tracking restore fails** (SynScan, issue #830): once the axes have stopped, AbortSlew clears the motion state before it re-sends the tracking mode, so a timed-out tracking write throws a `DriverException` with `Slewing` already false (test `SynScan AbortSlew - a failed tracking restore still clears Slewing (#830)`).
