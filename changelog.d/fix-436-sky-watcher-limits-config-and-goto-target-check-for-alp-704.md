### Added

- **Sky-Watcher direct: optional minimum-altitude and meridian limits, and a goto target check** (issue #436). New per-device `minAltitudeDeg` and `meridianLimitMinutes` settings, off unless set, with web UI fields (blank = off). A `SlewToCoordinates`, `SlewToCoordinatesAsync`, `SlewToTarget` or `SlewToTargetAsync` to a target below the altitude floor is refused with `InvalidValue` naming the Minimum altitude limit setting; Park, FindHome, MoveAxis and Sync are exempt. The limit is soft: set it at least 3 degrees above the real clearance. ConformU runs with the limits off.
