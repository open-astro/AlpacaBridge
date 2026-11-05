### Fixed
- **QHY Q-Focuser: a reconnect right after a move no longer waits 3 s and fails** (AlpacaCore, issue #2041): the connect read the supply telemetry with the full serial timeout, and the firmware answers no telemetry for about 10 s after a move. The connect now uses the poll's 500 ms deadline; a miss defers the 12 V hold settings to the first telemetry reading the poll gets and applies them once. Reverse and speed still go out at connect.
- **QHY Q-Focuser: Position and IsMoving no longer lag one poll on the one-reply-behind firmware** (AlpacaCore, issue #2042): when a command's idx repeats the previous reply's (back-to-back position polls), the wrapper reads on for the kicked fresh reply for up to 50 ms and returns it instead of the stale one, so an arrival shows on its own tick.

### Added (tests)
- **Q-Focuser connect-in-silence and one-behind poll cases** (issue #2041, issue #2042): five `[snapshot]` cases on the pty fake and the fake clock, the hold write that times out at connect included.
