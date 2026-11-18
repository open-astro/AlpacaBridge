### Added (internal)
- **Per-axis motion owner** (AlpacaCore, issue #2263): `util/motion_owner.h` holds one policy for what a new motion command does to motion already running: a command owns only the axes it moves, a PulseGuide during a slew, park or home is refused, and a stop cancels only its own axes. No driver uses it yet, so nothing changes for clients.

### Added (tests)
- **Motion owner policy table** (issue #2263): the (running command, new command) to outcome table, stops, superseded-over-cancelled precedence, the PulseGuide refusal and a body in a slot on the task clock.
