### Fixed
- **Bisque AbortSlew no longer stalls every getter behind a pulse guide** (AlpacaCore, issue #887): `abort_slew` releases the driver mutex before it waits on the TheSkyX socket, so `Slewing` and the other getters answer while a guide is pending (test `Bisque AbortSlew - a getter does not stall behind a pending pulse guide`). The abort is still sent only after the guide reply, never on the busy socket.

### Changed
- **Bisque PulseGuide `Duration` is capped at 30000 ms** (AlpacaCore, issue #887): a longer duration is refused with `InvalidValue` (it was bounded only by `INT_MAX`, about 24.8 days of blocked socket); 30000 is accepted (test `Bisque PulseGuide - the maximum duration is accepted and one more is InvalidValue`).
