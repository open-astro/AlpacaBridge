### Breaking changes
- **Bisque `PulseGuide` with a `Duration` above 30000 ms is now refused with `InvalidValue`** (AlpacaCore, issue #887): Bisque telescope clients that sent a longer `Duration` had the call accepted before (test `Bisque PulseGuide - the maximum duration is accepted and one more is InvalidValue`). **After upgrading:** send guide pulses of at most 30 s.

### Fixed
- **Bisque AbortSlew no longer stalls every getter behind a pulse guide** (AlpacaCore, issue #887): `abort_slew` releases the driver mutex before it waits on the TheSkyX socket, so `Slewing` and the other getters answer while a guide is pending (test `Bisque AbortSlew - a getter does not stall behind a pending pulse guide`). The abort is still sent only after the guide reply, never on the busy socket.

### Changed
- **Bisque PulseGuide `Duration` is capped at 30000 ms** (AlpacaCore, issue #887): it was bounded only by `INT_MAX`, about 24.8 days of blocked socket; see Breaking changes.
