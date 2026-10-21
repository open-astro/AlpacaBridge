### Fixed

- **OnStep: the `:GU#` pier side is read from `T`/`W`/`o`, not from the mount-type `E`** (OnStep, issue #1217): OnStepX writes `E` for a German equatorial mount on every status reply and the pier side as `T` (east), `W` (west) or `o` (none), but `parse_status()` read `E` as "pier east", so `MountStatus::side_of_pier` was always east on a GEM; no ASCOM property uses that field (`SideOfPier` is computed from hour angle), so clients see no change.

### Changed (docs)

- **OnStep instructions: `:GU#` letters and the PulseGuide notes corrected** (OnStep, issue #1217): the status table now lists the mount-type letters, the real pier-side letters, `I`/`F`/`h`/`e` and the three trailing digits (pulse-guide rate, guide rate, general error, with error 7 `ERR_MERIDIAN` and how to clear it); the SideOfPier note no longer explains the old `EE` ConformU failure as the physical pier side; and the PulseGuide note separates the North/South issues at HA ±9, Dec +85 (refraction in OnStepX's reported coordinates, proven on the TeSeek HM-17, upstream ASCOMInitiative/ConformU#35) from the East/West noise, and corrects the reading of ConformU's `-9.0`/`+9.0` labels, which are hour angles, not guide rates.

### Added (tests)

- **OnStep `:GU#` pier-side case** (issue #1217): real HM-17 status replies decode to pier west, pier east and no pier side, over the loopback fake mount.
