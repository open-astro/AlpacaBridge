### Fixed
- **Bisque (TheSkyX): PulseGuide refused on an axis MoveAxis is moving** (AlpacaCore, issue #2503): a guide pulse on the axis a non-zero MoveAxis drives sent a DirectGuide into the running jog; it is now refused with InvalidOperation from the jog's start until that axis is stopped, and the jog keeps running. A pulse on the other axis is still accepted.
