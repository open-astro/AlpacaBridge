### Fixed

- **OnStep: the tracking restore can no longer fire at an unrelated slew** (OnStep, issue #1230): the flag a coordinate GOTO sets to restore tracking was only used and cleared when a `Slewing` poll saw that slew end, so a synchronous `SlewToCoordinates` (whose wait loop hides that edge), an `AbortSlew`, or a client's `Tracking = false` during the slew left it set, and it could start tracking at the end of a later slew the driver did not start (a handset GOTO) if the mount was not tracking then; one helper now makes the decision on every path that ends a GOTO (the async edge, the end of a synchronous slew, and `AbortSlew`, which restores tracking at once when the mount lost it, as ASCOM requires), and every `Tracking` write clears the flag.

### Added (tests)

- **OnStep restore-flag cases** (issue #1230): a synchronous GOTO then a foreign slew, a synchronous GOTO that loses tracking, `Tracking = false` during a GOTO, and `AbortSlew` with tracking kept or lost, each followed where it matters by a foreign slew that ends untracked, over the loopback fake mount.

### Added (docs)

- **OnStep: ConformU report for the TeSeek HM-17** (OnStep): `AlpacaCore/conformu/OnStep/TeSeek HM-17/Linux-arm64.txt`, ConformU 4.5.2-rc.1 (build 56111) on the ASIAIR Plus against localhost, 0 errors, 0 issues and no timing findings; site coordinates rounded to the nearest degree. The four PulseGuide North/South issues at HA ±9 that earlier runs showed are gone: ConformU 4.5.2 keeps its test declinations within ±65° (ASCOMInitiative/ConformU#35). `SUPPORTED-DRIVERS.md` gains the row, and the README count goes to 86.
