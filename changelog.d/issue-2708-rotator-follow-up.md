### Fixed
- **remotealpaca Rotator: a move reads IsMoving true at once even when the poll's frame says it is already there** (AlpacaCore, issue #2708, follow-up to #2742): `MoveAbsolute` and `MoveMechanical` always write `IsMoving` true through, and a relative `Move` unless its offset is 0. The frame's angle can be a poll period old (another client, a hand controller or a coasting `Halt` may have moved the rotator), so a move to that angle no longer reads `IsMoving` false while the upstream moves.
- **remotealpaca Rotator: `CanReverse` never throws `NotConnected` during a Disconnect** (AlpacaCore, issue #2708): the connected or the disconnected answer is picked under one state-lock hold.

### Added (tests)
- **remotealpaca caller-order rendezvous** (issue #2708): `UpstreamProxy` gains a test-only command waiting hook (`set_command_waiting_hook_for_testing()` for the Rotator and the ObservingConditions device). The Rotator Halt ordering case and the three ObservingConditions ordering cases wait on it instead of a 300 ms real-time window, so each fails every time without the command order mutex. A new `[stress]` case races `CanReverse` against connects and disconnects.
