### Changed
- **Bisque telescope: PulseGuide, MoveAxis, the equatorial slews and the guide-rate setters run their checks through the telescope command gate** (AlpacaCore), issue #2258. No error code, message, limit or order changes: every family keeps checking the link fault before the parked check.
