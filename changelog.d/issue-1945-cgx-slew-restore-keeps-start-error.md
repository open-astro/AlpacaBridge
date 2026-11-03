### Fixed
- **Celestron CGX: a failed slew start keeps its own error** (AlpacaCore, issue #1945): when `SlewToCoordinatesAsync` or `SlewToTargetAsync` could not start the slew and turning tracking back on also failed, the client got the tracking error (for example "AUX transaction timed out") in place of the reason the slew did not start. The client now gets the original slew error, and the tracking failure is logged at error level; Tracking still reads off or unknown afterwards, as before.

### Added (tests)
- **CGX slew-start tracking restore case** (issue #1945): one fake-mount case with two sections, the restore failing and the restore succeeding, and an acknowledgement-with-payload fault in `FakeCelestronCgxMount`.
