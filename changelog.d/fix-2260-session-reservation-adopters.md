### Fixed

- **ZWO and SVBONY cameras no longer drop an AbortExposure sent while a StartExposure is still starting**, ZWO and SVBONY camera drivers, issue #2260. A StartExposure can wait up to 2 s in the capture session behind an earlier body the watchdog left in the SDK; an AbortExposure in that window returned and the new exposure then ran to its end and served a frame. The abort is now recorded and ends that start as it publishes, so no exposure runs (`ZWO camera clock - an AbortExposure during a StartExposure still in flight is not dropped`, `SVBONY Camera Driver - an AbortExposure during a StartExposure still in flight is not dropped`).

### Changed

- **ZWO, SVBONY and ToupTek (and Altair) use the capture session's start reservation**, issue #2260 ([decision 0020](docs/decisions/0020-capture-session.md) rule 9). Each driver drops its own `exposure_starting_` flag (and ZWO and SVBONY their `StartingDone` guard, ToupTek its `abort_pending_` flag): the start reserves under the driver mutex, the setters ask the session, and an abort in the start window is recorded by the session. A settings write during an exposure now reads "<camera>: cannot change camera settings during an exposure" on every camera (ZWO said "ROI"). ConformU not re-run.
