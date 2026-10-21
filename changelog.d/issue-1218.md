### Fixed

- **OnStep: FindHome no longer restarts tracking and drives the mount off home** (OnStep, issue #1218): when any slew finished, the driver turned sidereal tracking back on if a flag said tracking had been on before, but that flag started out true at connect and `park()`/`find_home()` never cleared it, so `:ST60.164275#` went out the moment a home or park completed; OnStepX ignores it while parked, but after FindHome the unparked mount started tracking away from home (seen on a TeSeek HM-17). Park and FindHome now clear the flag, it starts false at connect, and after a coordinate slew it restores tracking once, only when the mount reports that it lost tracking during the slew.

### Added (tests)

- **OnStep tracking-restore cases** (issue #1218): FindHome and Park send no tracking command when they complete, and a coordinate slew sends one only when the mount stopped tracking during it, over the loopback fake mount.
