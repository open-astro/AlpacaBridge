### Fixed

- **A focuser `Move` outside 0..MaxStep goes to that limit instead of being refused** (iOptron iFR, iOptron iEAF/iAFS, QHY Q-Focuser, ToupTek AAF, ZWO EAF; issue #2028): ConformU's focuser tester moves below 0 and above `MaxStep` and expects the focuser to stop at the limit, as the Gemini, Celestron and Astroasis focusers already did; the five drivers threw `InvalidValue` and passed validation only because every ConformU release up to 4.6.0-rc.1 skipped the range sweep for a focuser without temperature compensation (fixed upstream in ConformU `a226f298`, 2026-10-28; a build of ConformU `main` on the iFR 3" showed the two Issues, and the sweep's other legs and halt-while-moving passed). Each driver now clamps the target to `0..MaxStep` after its connection check; the ZWO EAF still refuses every Move while the unit's stored max step is 0 and none is configured, a Move to 0 included (the old range check let that one through). For a client that sent such a position, see Breaking changes.

### Breaking changes

- **A focuser `Move` below 0 or above `MaxStep` now moves the focuser to that limit** (iOptron iFR, iOptron iEAF/iAFS, QHY Q-Focuser, ToupTek AAF, ZWO EAF; issue #2028): a client that relied on the `InvalidValue` refusal to catch a bad position gets a successful move to 0 or to `MaxStep` instead. No config change; send positions inside `0..MaxStep`.

### Added (tests)

- **Focuser clamp cases** (issue #2028): the per-driver range cases assert the limit on the wire ("A move outside 0..MaxStep goes to that limit" in the iEAF, iFR and Q-Focuser files, and the reworked ToupTek and ZWO range assertions), and the five sweep entries are `with_clamping_focuser`; the sweep's `with_static_focuser_range` and its `move(-1)` probe are gone with their last users.
