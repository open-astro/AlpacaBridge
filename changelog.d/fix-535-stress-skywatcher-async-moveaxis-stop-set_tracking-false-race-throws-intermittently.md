### Fixed

- **Sky-Watcher SetTracking(false) during a MoveAxis stop**: `SetTracking(false)` issued while a `MoveAxis(axis, 0)` stop is still ramping down no longer throws "Tracking change superseded by a concurrent motion command" and no longer leaves the mount tracking; the background restore yields to the in-flight command (issue #535; `SkyWatcher async - MoveAxis stop restore yields to a newer tracking command`, `MoveAxis stop racing SetTracking(false)`).
