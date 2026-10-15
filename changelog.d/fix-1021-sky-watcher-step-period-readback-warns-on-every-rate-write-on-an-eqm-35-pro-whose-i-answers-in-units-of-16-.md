### Fixed

- **Sky-Watcher EQM-35 Pro: the log no longer fills with "step period readback" warnings on every rate write** (SkyWatcher, issue #1021): the board answers the `:i` readback in units of 16 ticks, signed by direction, so the driver now accepts that encoding as a match; a readback that fits neither encoding still warns (`test_skywatcher_serial.cpp`, "a signed /16 ':i' readback does not warn" and "fitting neither encoding still warns").
