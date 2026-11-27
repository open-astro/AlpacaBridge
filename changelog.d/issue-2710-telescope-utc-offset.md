### Fixed
- **remotealpaca Telescope: UTCDate through `util::UtcOffset`** (AlpacaCore, issue #2710, follow-up to #2759): the upstream's UTCDate in the polled frame, and a UTCDate this server writes, are anchored on the task clock where they were read or written (decision 0014), and read back advanced by the task-clock time since. The value served does not change; the readback is now tested on the fake task clock. Decision 0014 gains a remotealpaca telescope row.
- **remotealpaca Telescope: a MoveAxis cap counts every other axis** (AlpacaCore, issue #2710): while this server's MoveAxis ran on axis 2, a cap on axis 0 or 1 read the mount's `Slewing` as true, resent its stop and raised a "stop not verified" error although its own axis had stopped (and an axis-2 cap did the same while axis 1 ran). The verified stop now counts this server's MoveAxis on any other of the three axes.

### Added (tests)
- **remotealpaca Telescope** (issue #2710): a UTCDate write reads back advanced by task-clock time; an axis-0 cap verifies while this server's axis-2 MoveAxis runs. `AlpacaUpstreamScript` moves axis 2 when its `CanMoveAxis(2)` is scripted true.
