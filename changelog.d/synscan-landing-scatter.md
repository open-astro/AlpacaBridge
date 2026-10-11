### Fixed

- **SynScan: a landing left off target after the refinement passes no longer fails the slew** (SynScan, issue #1027): 5.0.0-beta.1 failed `SlewToCoordinates`, `SlewToTarget` and their async forms with a `DriverException` when the GOTO was still more than 10 arcseconds off after the third refinement pass. An EQM-35 Pro on its HC 06.03.00 handset lands each GOTO with about ±10 arcseconds of random Dec scatter (up to 20), so slews within plate-solving range failed at random, and ConformU stopped part-way through. The driver now logs the residual as a WARN and completes the slew, as 4.2.0 did and as it already does for a miss of more than 600 arcseconds. Refinement still cancels a systematic offset (10.4 arcseconds in RA to 0.1 on the rig), and a GOTO the handset refuses, or a disconnect, still fails the slew.

### Added (tests)

- **SynScan landing scatter cases** (issue #1027): two cases in `test_synscan_goto_landing.cpp` (sync and async) replay the Dec scatter the rig logged per GOTO, through a scripted per-GOTO Dec offset in the fake handset. The two #880 non-convergence cases now expect the WARN instead of the `DriverException`.
