# Sky-Watcher pointing clock and UTCDate readback

Status: accepted

## Context

The direct motor-controller mount has no independent clock. Before #287, a client's `UTCDate` write changed the ASCOM readback but not the host-clock LST used for pointing. Applying every client offset fixed that mismatch, but #301 showed that a mis-set tablet could then move pointing by 7.5° on an NTP-disciplined rig.

## Decision

`client_utc_now_locked()` honours a client's surviving write for the ASCOM readback. `utc_now_locked()` uses the client offset for LST and goto only while the host is undisciplined; once the host is disciplined, pointing uses its clock. A system-clock step invalidates the stored offset by comparison with elapsed steady-clock time. Discipline is re-sampled while an offset is armed, so a host that later acquires NTP stops using the client offset. This change is one-way for that write.

## Alternatives rejected

Always use the raw host clock: an undisciplined field rig can retain a wrong pointing clock while reporting the client's corrected time. Always apply the client offset: a bad client can spoil a good NTP clock. Reject `UTCDate`: it breaks the expected ASCOM write/readback behavior.

## Consequences

Readback and pointing time can deliberately differ on a disciplined host. The clock policy must be tested under both host-discipline states through the probe seam, not whichever state the CI runner happens to have.

## Links

- Issues [#287](https://github.com/open-astro/AlpacaBridge/issues/287), [#301](https://github.com/open-astro/AlpacaBridge/issues/301); PRs [#291](https://github.com/open-astro/AlpacaBridge/pull/291), [#351](https://github.com/open-astro/AlpacaBridge/pull/351).
- Implementation: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` (`utc_now_locked()`, `client_utc_now_locked()`).
- Regression tests: `AlpacaCore/tests/test_skywatcher_async.cpp` cases for the #287/#301 branches, the NTP-disciplined host, UTCDate readback, and an external clock step.
