### Fixed
- **ZWO AM: PulseGuide stays refused during a GOTO that a jog interrupted as it started** (AlpacaCore, issue #2459): a MoveAxis jog that landed while SlewToTargetAsync was starting took the slew's motion claim, so PulseGuide was accepted while that GOTO started and for its whole flight; it is now refused with InvalidOperation until the slew call returns and while the driver's own GOTO is in flight.
- **ZWO AM: PulseGuide refused on an axis MoveAxis is moving** (AlpacaCore, issue #2459): a guide pulse on the axis a non-zero MoveAxis drives sent `:Mg` into the running jog; it is now refused with InvalidOperation from the jog's start until that axis is stopped. A pulse on the other axis is still accepted.

### Added (tests)
- **ZWO AM PulseGuide refusal and live-drain destruction cases** (issue #2459): a jog in a GOTO's claim gap, a pulse on a jogging axis (and in a jog's start), and destruction with both pulse drains live.
