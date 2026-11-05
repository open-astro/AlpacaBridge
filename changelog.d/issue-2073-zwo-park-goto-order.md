### Fixed
- **ZWO AM mount: a SlewAsync can no longer start its GOTO after a Park, FindHome, MoveAxis or AbortSlew command it raced** (AlpacaCore, issue #2073): these four commands revoked any pending GOTO and then sent their own command with no lock held, so a SlewAsync from another client could claim in that gap, return first, and send `:MS` after `:hP`, `:hC`, the jog or `:Q`, leaving the mount slewing to the target. They now hold the driver's initiator lock from before the revocation until their own command is sent, so a racing SlewAsync waits for the command and is ordered after it. Unpark was already safe (a SlewAsync is refused while the mount is parked).

### Added (tests)
- **ZWO initiator ordering cases** (issue #2073): a SlewAsync in each fencing initiator's gap waits for its command; an AbortSlew during a Park stops after `:hP` and returns within the 1 s budget; Connected=false returns while a Park holds the lock; a SlewAsync during Unpark's gap is refused as parked.
