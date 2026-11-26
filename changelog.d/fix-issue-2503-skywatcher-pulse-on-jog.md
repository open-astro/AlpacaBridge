### Fixed
- **Sky-Watcher: PulseGuide refused on an axis MoveAxis is moving** (AlpacaCore, issue #2503): a guide pulse on the axis a non-zero MoveAxis drives was accepted, and its body stopped the jog and re-commanded the axis at the guide rate; it is now refused with InvalidOperation, nothing is sent and the jog runs on. It is refused from the jog's claim, over its dispatch stop-wait, until its stop is sent. A pulse on the other axis is still accepted.
- **Sky-Watcher: PulseGuide stays refused when a jog lands as a GOTO or FindHome starts** (AlpacaCore, issue #2503): a MoveAxis between the slew's start and its body took an axis from the slew claim, so a pulse on the other axis was accepted while Slewing read true and the body was still to dispatch both axes; it is now refused with InvalidOperation.

### Added (tests)
- **Sky-Watcher PulseGuide refusal cases** (issue #2503): a pulse on a jogging axis and in a jog's dispatch stop-wait, and a pulse after a jog in a GOTO's or FindHome's start gap.
