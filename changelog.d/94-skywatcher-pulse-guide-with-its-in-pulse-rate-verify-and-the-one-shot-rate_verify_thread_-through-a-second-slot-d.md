### Changed
- **Sky-Watcher telescope: pulse guide and the RA rate-applied check run on operation slots** (AlpacaCore, issue #943): each axis's PulseGuide body has its own `util::AsyncOperation` slot and the one-shot check after a `RightAscensionRate` or `TrackingRate` write has a slot of its own, replacing `pulse_task_thread_`, `pulse_task_cancel_`, `rate_verify_thread_` and their reap code; every wait goes through the task clock. A rate check's resend now runs under the driver mutex and only while its start epoch is current, so a cancel that lands after the check's last wait can no longer put `:I`+`:J` into an axis a stop or pulse now owns (`test_skywatcher_async.cpp`).

### Added (tests)
- **Sky-Watcher pulse cases on virtual time** (issue #943): `IsPulseGuiding` clears with the pulse's stop (#559) and a reconnect after a mid-pulse disconnect leaves no axis running (#521).
