### Fixed

- **ZWO camera Disconnect forgets the camera id along with its open**, ZWO camera driver, issue #1667. `release_open()` now resets the held camera id to -1 with the link, so a call still holding the old id after Disconnect is refused by the driver ("Camera not connected", NotConnected) before it reaches the shared open table.

### Added (tests)

- **ZWO dew heater test fake checks links like the library**, issue #1667. `FakeDewHeaterSdk` opens, closes and runs `with_link()` through its own `SharedOpenTable`: a caps, value or serial call outside `with_link()` is a DriverException ("no link for id 3 on this thread") and a closed link is NotConnected, so dropping `with_link()` around the dew heater write or listing fails "a write and a reconnect drop the cached frame" and "a serial binds its camera over a conflicting index". Its remaining differences are in a KNOWN PARITY GAPS block. A new case, "a camera removal error stops the switch before the SDK", shows a camera-side removal error retiring the shared open and closing the dead handle, and the dew heater switch's next read answering NotConnected ("stale link") before the SDK.

### Changed

- **ZWO ASI6200MM Pro and dew heater switch ConformU reports**, issue #1667. ConformU 4.5.3 on Linux arm64 (Armbian trixie), 2026-11-01, with the camera and its dew heater sharing one open: 0 errors, 0 issues, every member within its timing target for both devices. SUPPORTED-DRIVERS.md names the run.
