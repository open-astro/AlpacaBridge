### Fixed

- **OnStep: AlignmentMode now matches the mount** (OnStep driver, issue #2214): the driver always reported a German equatorial mount. It now reads the mount type OnStepX and OnStep 4.x report in their status: a fork mount reports `Polar`, and an alt-az or alt-alt mount reports `AltAz` (ASCOM has no alt-alt value), so clients no longer plan meridian flips for a mount that has none. A German mount, firmware that reports no mount type, and a disconnected driver still report `GermanPolar`. Not yet checked on a fork or alt-az mount.

### Added (tests)

- **OnStep AlignmentMode cases** (issue #2214): the status parser reads the mount type, and the driver answers `AlignmentMode` for fork, alt-az, alt-alt, German and no-type frames, and reports `GermanPolar` after a disconnect.
