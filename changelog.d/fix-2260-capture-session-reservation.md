### Fixed

- **Player One cameras no longer drop an AbortExposure sent while a StartExposure is still starting**, Player One camera driver (iCAM rides it), issue #2260. A StartExposure can wait up to 2 s in the capture session behind an earlier body the watchdog left in the SDK; an AbortExposure in that window returned and the new exposure then ran to its end and served a frame. The abort is now recorded and stops the exposure once it runs (`Player One Camera Driver - an AbortExposure during a StartExposure still in flight is not dropped`).

### Changed

- **The capture session owns the start reservation**, `util::CaptureSession`, issue #2260 ([decision 0020](docs/decisions/0020-capture-session.md) rule 9). A camera reserves its start under its own mutex while it reads its settings (`reserve()`), the session keeps the settings locked until the exposure is published or the start is refused (`settings_locked()`, `ensure_settings_unlocked()`), a disconnect drops the reservation, and an abort in the window is recorded and reported by `start()`. Player One moves onto it and drops its own `exposure_starting_` flag and `StartingDone` guard; a settings write during an exposure now reads "PlayerOne: cannot change camera settings during an exposure". ZWO, SVBONY and ToupTek move in the next slice. ConformU not re-run.
