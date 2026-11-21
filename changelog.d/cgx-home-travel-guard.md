### Fixed
- **Celestron CGX: a home search that drives an axis too far is stopped** (AlpacaCore, private, no issue): during FindHome, Park and a pier-side-changing slew the driver now adds up how far each axis has turned, the #2213 re-home included. An axis past 270 degrees is stopped and the command fails with an error Slewing keeps reporting, and an axis that has already turned more than 90 degrees by the 60 s re-home point is stopped instead of searched again. Before, a search that never found its switch could turn an axis about 380 degrees before it timed out (the 2026-10-28 incident on a bare CGX). USB and SkyPortal WiFi alike.

### Added (tests)
- **CGX home travel cases** (private, no issue): the fake mount runs scripted home searches (legs that end at home, or a runaway); driver cases for the incident shape on FindHome, Park and the meridian route, a re-home that runs away, the longest legitimate homes, and one runaway over the fake SkyPortal adapter.
