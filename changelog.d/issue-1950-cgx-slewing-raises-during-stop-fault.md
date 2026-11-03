### Fixed
- **Celestron CGX: Slewing reports the link fault while a stop is pending** (AlpacaCore, issue #1950): after AbortSlew or Unpark on a link that had stopped answering, `Slewing` read `true` until the mount answered again or the stop failed. It now raises the `DriverException` link fault like every other read (decision 0009 rule 1). On a healthy link a pending stop still reads `true`.

### Added (tests)
- **CGX pending-stop link fault case** (issue #1950): one test case for AbortSlew and Unpark; six existing CGX cases that used a `Slewing` read to wait for a stop on a faulted link now wait through the fault or for the stop's own end.
