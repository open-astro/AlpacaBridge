### Breaking changes

- **Bisque/TheSkyX: a PulseGuide during a slew or FindHome is refused** (AlpacaCore, issue #2263): it answers `InvalidOperation` ("PulseGuide is not allowed while a slew, park or home is running") and sends nothing, where it used to be accepted and its DirectGuide went out once the slew command or the home loop let go of the socket. A PulseGuide during a Park was already refused (`InvalidWhileParked`) and still is. The refusal lasts while Slewing is held for a slew (until TheSkyX reports it complete, or AbortSlew) and while a FindHome call runs. **After upgrading:** a client waits for Slewing to read false, and for FindHome to return, before it pulse-guides, as ConformU and the guiding programs already do.

### Fixed

- **Bisque/TheSkyX: a FindHome taken over or stopped mid-home no longer sets AtHome** (AlpacaCore, issue #2263): a MoveAxis, Park or AbortSlew sent while TheSkyX ran the home loop left AtHome true once the loop answered (Unpark, `MoveAxis(axis, 0)` and a disconnect now cancel a FindHome the same way). The driver's Park and slew counters are replaced by the per-axis motion owner (`util::MotionOwner`): a slew, Park or FindHome claims both axes, a MoveAxis its own, and a stop cancels, so a FindHome or Park that lost its claim writes no state, as a cancelled Park already did (#2062).
