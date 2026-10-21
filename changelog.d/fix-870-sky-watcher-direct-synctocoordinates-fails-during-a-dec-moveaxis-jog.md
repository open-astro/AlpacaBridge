### Fixed

- **Sky-Watcher direct: SyncToCoordinates during a MoveAxis jog now answers a clear error** (SkyWatcher, issue #870): sync used to write the position to a motor MoveAxis was still turning, so the client saw the controller's "Motor not stopped"; it now refuses with `InvalidOperation` naming the jogging axis before any state changes and leaves the jog running (cases in `test_skywatcher_async.cpp`).
