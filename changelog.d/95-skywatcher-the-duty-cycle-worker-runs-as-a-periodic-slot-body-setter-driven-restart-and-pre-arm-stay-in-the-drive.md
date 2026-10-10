### Changed
- **Sky-Watcher telescope: the sub-floor duty-cycle worker runs on an operation slot** (AlpacaCore, issue #949): both axes' duty-cycle bursts now run as one long-lived 50 ms periodic body on a `util::AsyncOperation` slot of its own, replacing `duty_thread_`, `duty_lifecycle_mutex_`, `reap_duty_task` and the driver's private `task_wait_for`; the setter-driven restart and the deferred-write pre-arm stay in the driver and Alpaca clients see no change.

### Added (tests)
- **Sky-Watcher deferred RA duty-cycle continuity** (issue #949): a sub-floor `RightAscensionRate` written while a pulse owns the RA axis resumes duty-cycling once the pulse ends (`test_skywatcher_async.cpp`).
