### Changed
- **iOptron telescope: PulseGuide, MoveAxis, the equatorial slews and the guide-rate setters run their checks through the telescope command gate** (AlpacaCore), issue #2258. No error code, message, limit or order changes; the discrete MoveAxis speeds and the calibrated guide-rate range stay the driver's own checks.
