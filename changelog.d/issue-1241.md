### Fixed

- **OnStep: a pulled or re-enumerated serial adapter is a lost link** (OnStep, issue #1241): with the USB cable out the driver kept `Connected` true, every command failed with the generic "Failed to send command to mount" (1280), a reply cut off mid-read waited out the response timeout, and a client's `Connected = true` did nothing while the dead fd stayed open; now the wrapper notices that the configured port path no longer reaches the node it opened (or that a read or write failed with `EIO`, `ENXIO`, `ENODEV` or `EBADF`), closes the fd, and the driver reports `Connected` false, an empty `DeviceState`, `NotConnected` at once and the reason through the link fault; the next `Connected = true` reconnects, and when the link was last seen alive 5 s or more before the reconnect completes (`util::kRelinkMotionPreserveWindow`, #521; the reconnect's own time counts, including an auto-detect re-scan) the reconnect first sends AbortSlew's stops, because OnStepX keeps running a GOTO while the link is out (and a `MoveAxis` for up to about 10 s).

### Added (tests)

- **OnStep link-loss cases over a pty fake** (issue #1241): unplug with no client I/O, a reply cut off mid-read, a command after an unnoticed unplug, a reconnect on a lost link, the #521 stop with and without a long outage (including the router's own `Connected` read before the reconnect), a reconnect that auto-detects the replugged adapter under a new port name, and a loss found while a command holds the port.

### Changed (docs)

- **Decision 0009's adoption table, AGENTS.md and the OnStep instructions** (issue #1241) record that OnStep follows the Sky-Watcher link-loss shape, and how OnStep times the #521 outage.
