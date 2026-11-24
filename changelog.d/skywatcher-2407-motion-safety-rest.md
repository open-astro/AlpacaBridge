### Fixed
- **Sky-Watcher Telescope Driver** (AlpacaCore, issue #2407): an AbortSlew whose stop check could not start used to leave Slewing reading false with nothing confirming the stop. Slewing now reports that the stop could not be confirmed, and the next connect still sends a stop.

### Breaking changes
- **Sky-Watcher Telescope Driver** (AlpacaCore, issue #2407): SyncToCoordinates is refused with InvalidValue when the new position is more than 5 degrees on the sky from where the mount points. The first Sync after each Connect is exempt, since the board has no sky reference at power-up; it sets the reference. **After upgrading:** do the large first Sync (or plate-solve Sync) right after connecting; a later Sync that is more than 5 degrees off needs a reconnect first.
