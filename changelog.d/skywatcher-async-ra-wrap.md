### Fixed

- **Sky-Watcher pulse-supersede test no longer fails when RA crosses 0h**, tests. "SkyWatcher async - a pulse superseded in its release's stop-wait keeps the MoveAxis rate (#2431)" subtracted two RA readings without wrapping, so it failed for a few minutes each sidereal day, when the reading crossed 24 h (main CI on 9678fbee1: `reported_moved := -358.99`). The case now sets UTCDate so the RA crosses 0h on every run, and takes the difference into -180..180 deg. No driver change.
