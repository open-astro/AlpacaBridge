### Fixed
- **Celestron NexStar: PulseGuide refused on an axis MoveAxis is moving** (AlpacaCore, issue #2503): a guide pulse on the axis a non-zero MoveAxis drives sent its `MC_AUX_GUIDE` into the running jog, so the motor ran at neither rate; it is now refused with InvalidOperation before anything is sent, and the jog keeps running. A pulse on the other axis is still accepted.

### Added (tests)
- **NexStar PulseGuide on a jogging axis case** (issue #2503): a pulse on the jogging axis is refused and sends nothing, and a pulse on the other axis is accepted, on both axes.
