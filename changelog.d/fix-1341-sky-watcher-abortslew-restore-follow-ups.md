### Fixed

- **Sky-Watcher `AbortSlew` during a `MoveAxis` stop restarts tracking once** — Sky-Watcher telescope driver, issue #1341. When `AbortSlew` landed while a `MoveAxis(axis, 0)` stop was still polling, both restored tracking, so RA was stopped and restarted a second time. `AbortSlew` now cancels the pending stop, so RA sees one stop and one restart.
