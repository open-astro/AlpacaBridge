### Changed
- **Celestron CGX telescope: PulseGuide, the equatorial slew and the guide-rate setters check their arguments through the telescope command gate** (AlpacaCore), issue #2258. Only the argument checks move; the state checks keep the CGX's own order. No error code, message, limit or order changes.
