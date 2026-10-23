### Fixed

- **OnStep Park and FindHome during a MoveAxis tests no longer fail under load**, tests, issue #1558. The test helper that steps the fake clock now waits for the Park or FindHome body's own clock wait, not for the MoveAxis re-send waiter that is still on the clock. A missed wait fails the test instead of adding fake time. Covered by `OnStep Telescope Driver - Park during a MoveAxis stops the move and parks` and `OnStep Telescope Driver - FindHome during a MoveAxis stops the move and homes` (400 ms and 1 s bounds unchanged).
