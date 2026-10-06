### Fixed

- **Sky-Watcher direct: the minimum-altitude and meridian limits now stop MoveAxis and tracking** (AlpacaCore, issue #824). With a limit set, a guard polls the axes every 250 ms while MoveAxis drives one and every 2 s while only tracking, and stops tracking and every MoveAxis axis the moment the motion carries the tube from at or above the altitude floor to below it, or the counterweight from inside the meridian limit to past it, with one WARNING log line naming the limit. Each stop is sent on its own, the MoveAxis stop first, and a stop that fails (a tracking stop that times out on a long deceleration ramp, a link error) is tried again at the next poll. Motion that starts outside a limit is not stopped, so the mount can always be driven back; gotos, Park and FindHome are not watched; with both limits off (the default) the guard never starts. A disconnect waits for the guard to finish.

### Added (tests)

- **Sky-Watcher live limit guard cases** (issue #824): seven cases over the loopback fake mount (MoveAxis below the floor, tracking past the meridian limit, motion starting outside a limit, limits off, disconnect during a guarded MoveAxis, a timed-out tracking stop that must not hold back the MoveAxis stop, a failed stop retried at the next poll), and the Sky-Watcher `[stress]` registration now runs with both limits set.
