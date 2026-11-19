### Changed
- **SynScan telescope: PulseGuide, MoveAxis, the equatorial slews and the guide-rate setters run their checks through the telescope command gate** (AlpacaCore), issue #2258. A PulseGuide call with both a bad direction and a negative duration now names the direction (the gate checks it first); the code is InvalidValue either way. No other error code, message, limit or order changes.
