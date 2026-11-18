### Breaking changes

- **Sky-Watcher: a PulseGuide during a slew or FindHome is refused** (AlpacaCore, issue #2263): it answers `InvalidOperation` ("PulseGuide is not allowed while a slew, park or home is running") and changes nothing, where it used to be accepted and its guide motion took the axis from the slew. A PulseGuide during a Park was already refused (`InvalidWhileParked`) and still is. **After upgrading:** a client waits for Slewing to read false before it pulse-guides, as ConformU and the guiding programs already do.

### Changed

- **Sky-Watcher: a MoveAxis no longer waits for the pulse on its axis to end** (AlpacaCore, issue #2263): a non-zero MoveAxis takes the axis from a running PulseGuide on the same axis at once and ends that pulse (IsPulseGuiding reads false), instead of joining its background body first (decision 0006 rule 3). The old pulse body sends that axis nothing more. A pulse on the other axis still runs on, and MoveAxis(axis, 0), AbortSlew, the slews, SyncToCoordinates and disconnect stop a pulse as before. The driver-wide motion counter is replaced by the per-axis motion owner (`util::MotionOwner`), so a one-axis command no longer loses its stop-wait to a command on the other axis.
