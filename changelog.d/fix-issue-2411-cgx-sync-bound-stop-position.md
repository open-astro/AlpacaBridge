### Breaking changes
- **Celestron CGX: a far Sync is refused** (AlpacaCore, issue #2411): SyncToCoordinates and SyncToTarget now fail with InvalidValue when the target is more than 5 degrees on the sky from the position the mount reports. Slew close to the object first (or FindHome and slew), then Sync. A Sync straight after FindHome to a point far from the pole is refused.
- **Celestron CGX: AbortSlew and Unpark report done about 0.5 s later** (AlpacaCore, issue #2411): Slewing stays true until the encoders hold still for 500 ms after the motors report the stop.

### Fixed
- **Celestron CGX: Sync is bounded** (AlpacaCore, issue #2411): a Sync more than 5 degrees from the served position is refused before any tracking or label write, so the reference, Tracking and the labels stay as they were. It uses the shared `util::check_sync_bound()`; for the new refusal, see Breaking changes.
- **Celestron CGX: the AbortSlew and Unpark stop checks the position** (AlpacaCore, issue #2411): the stop was verified when the status showed both axes stopped, which a moving axis can report. It now also needs two encoder reads 500 ms apart that agree within 30 arcsec, within the same 3 s budget; an axis still moving at 3 s, or a read that fails, fails the stop and Slewing raises it. The other stops (disconnect, a cancelled or failed motion) keep their 500 ms status check.

### Added (tests)
- **CGX goto-safety cases** (issue #2411): `[goto-safety]` cases for the Sync bound, the position-checked stop (still, never still, failed read), a pin that DestinationSideOfPier predicts the side a direct, home-route, meridian and retarget slew lands on, and a pin that a Connect after a link lost during a GoTo stops both axes before any motion.
