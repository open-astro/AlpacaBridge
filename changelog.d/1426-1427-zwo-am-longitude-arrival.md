### Breaking changes
- **ZWO AM mount longitude convention corrected** (AlpacaCore, issue #1426): mount-derived SiteLongitude now reports east-positive ASCOM coordinates, and writes convert to the mount's west-positive protocol. **After upgrading:** users who reversed a saved longitude to work around the old behavior must restore the actual geographic longitude (west negative, east positive) before commanding motion.
- **ZWO AM horizon rejection no longer flips the site longitude**: after one normal site/time refresh, a repeated `e5` rejection is reported as a slew failure. The driver no longer retries against a mirrored site.

### Fixed
- **ZWO slew completion publishes final coordinates with stopped status** (AlpacaCore, issue #1427): prevent a client from sampling the previous RA/Dec after Slewing becomes false; an incomplete or failed final coordinate read leaves completion pending.
- **ZWO AM mount site reads and writes invert protocol longitude correctly** (AlpacaCore, issue #1426): prevent disagreement between the mount's pointing model and the driver's sidereal time when connecting, changing site coordinates or preparing a GoTo; for saved workaround entries, see Breaking changes.

### Changed (validation)
- **ZWO AM5 firmware 1.8.6 validated on Linux arm64**: ConformU 4.5.2 completed with no errors, warnings, issues or timing violations on the stable/5.0-based build, including removal of the inverted-longitude retry; the redacted report is saved under `AlpacaCore/conformu/ZWO/AM5/Linux-arm64.txt`. The repeated-horizon-rejection path also has separate fake-mount regression coverage.
