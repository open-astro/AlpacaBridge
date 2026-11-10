### Fixed
- **ZWO AM mount: an older Sync can no longer stop a newer MoveAxis, FindHome, Park or Unpark** (AlpacaCore, issue #1925): a Sync that waits for the mount to settle (`e4`) stops it again only while no newer command has started. MoveAxis, FindHome, Park and Unpark marked themselves as newer only after their wire command, so a Sync check in that gap passed and its `:Q`, `:SMMC` or `:CM` went out after the newer command and stopped it. They now mark themselves under the target-write lock before the command, as a GOTO already did, and the superseded Sync throws InvalidOperation with "Sync superseded by a newer command" (it said "newer motion command", which was wrong after an AbortSlew).

### Added (tests)
- **ZWO older-Sync cases** (issue #1925): a Sync settling during a MoveAxis jog, FindHome, Park or Unpark sends nothing after the newer command and fails; an AbortSlew during the settle gives the neutral message. The test `Latch` moved to `driver_rig.h`.
