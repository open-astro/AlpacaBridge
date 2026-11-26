### Changed

- **Every mount driver's 60 s MoveAxis cap runs on the shared motion guard** (Bisque, NexStar, iOptron, OnStep, SynScan, Sky-Watcher, ZWO AM, issue #2651): each axis has a slot body, `util::run_motion_deadline()`, and `util::kMaxMoveAxisDuration` is the only 60 s constant. A cap stop the mount ignores ends after the guard's three stops and Slewing raises `util::kStopNotVerifiedText`, instead of Bisque, NexStar and ZWO retrying without end; iOptron stops and verifies each axis alone (`:qR#`/`:qD#`). Tests: the `[goto-safety]` MoveAxis cap cases in each driver's test file.
