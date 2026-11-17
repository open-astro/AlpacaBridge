### Fixed

- **Sky-Watcher PulseGuide, MoveAxis and the equatorial slews check their arguments first** (Sky-Watcher telescope, issue #2257): a bad direction, duration, axis, rate, RA or Dec answered `NotConnected` while disconnected and `InvalidWhileParked` on a parked mount, ahead of the argument check. They now throw `InvalidValue` in every state. The stop, fault and parked rules for good arguments are unchanged.
