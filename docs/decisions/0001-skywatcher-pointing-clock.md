# Sky-Watcher pointing clock and UTCDate readback

Status: accepted

## Context

The direct motor-controller mount has no independent clock. Before #287, a client's `UTCDate` write changed the ASCOM readback but not the host-clock LST used for pointing. Applying every client offset fixed that mismatch, but #301 showed that a mis-set tablet could then move pointing by 7.5° on an NTP-disciplined rig.

The first answer to #301 kept the client's write for the `UTCDate` readback and used the host clock for pointing once the host was disciplined. ASCOM requires more: "It is required for a mount to calculate [`SiderealTime`] from its source of UTCDate and SiteLongitude" ([Telescope](https://ascom-standards.org/newdocs/telescope.html)). With the split, a client could read a `SiderealTime` that its own `UTCDate` read does not explain (issue #1472). The same interface lets a `UTCDate` write throw `PropertyNotImplementedException`, and the ASCOM [time and place FAQ](https://ascom-standards.org/newdocs/mount-time-place-faq.html) describes a mount with a better time source refusing the write.

## Decision

The driver has one time source at a time, and `UTCDate`, `SiderealTime` and pointing (LST and goto) all read it.

- **Host clock disciplined (NTP-synced).** A `UTCDate` write throws `PropertyNotImplemented` and changes nothing. All three use the host clock.
- **Host clock undisciplined.** A `UTCDate` write is accepted and stores an offset. All three use the host clock plus that offset.
- A system-clock step invalidates the stored offset by comparison with elapsed steady-clock time.
- Discipline is re-sampled while an offset is armed. A host that later acquires NTP drops the offset, so all three move to the host clock together, and later writes are refused.

Decided 2026-10-21 on issue #1470. It replaces the earlier decision, under which `client_utc_now_locked()` returned the client's write for the readback while a disciplined host's pointing used the host clock.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. `UTCDate`, `SiderealTime` and pointing time (LST and goto) read the same time source, so a client can explain `SiderealTime` from its own `UTCDate` and `SiteLongitude` reads. Scope: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`. Checked by: review, until issue #1472 adds the test cases.
2. While the host clock is disciplined, a `UTCDate` write throws `PropertyNotImplemented` and changes nothing; while it is undisciplined, the write is accepted and the offset applies to all three. The discipline state is re-sampled while an offset is armed. Scope: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`. Checked by: review, until issue #1472 adds the test cases; `AlpacaCore/tests/test_skywatcher_async.cpp` holds the #287, #301 and NTP-disciplined cases.
3. A system-clock step, found by comparing system and steady elapsed time, invalidates the stored offset. Scope: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_async.cpp` (external clock step case).
4. Clock-policy tests set both host-discipline states through the probe seam, never the state of the runner. Scope: `AlpacaCore/tests/test_skywatcher_*.cpp`. Checked by: review.

## Alternatives rejected

- **Always use the raw host clock.** An undisciplined field rig can keep a wrong pointing clock while the client holds the correct time.
- **Always apply the client offset.** A bad client can spoil a good NTP clock (#301).
- **Keep the client's write for the readback and the host clock for pointing (the earlier decision).** `SiderealTime` then follows a time source that `UTCDate` does not report, which the Telescope interface forbids. The earlier record rejected refusing the write because "it breaks the expected ASCOM write/readback behavior"; the interface allows a refused write, so that reason does not hold.
- **Refuse the write with `InvalidOperation`.** `PropertyNotImplemented` is the exception the interface names for a mount that does not take a client's time.

## Consequences

- A client that writes `UTCDate` on a disciplined host gets `PropertyNotImplemented` instead of a stored value. How each client reacts to that is not checked here; the rig run for issue #1472 records what NINA does.
- Whether a write is accepted can change while the driver runs, when the host gains or loses NTP. A client that wrote once can be refused later.
- The clock policy must be tested under both host-discipline states through the probe seam, not whichever state the CI runner happens to have.
- The driver on `main` still follows the earlier decision until issue #1472 lands. That change needs a ConformU run on the rig: ConformU accepts `PropertyNotImplemented` from the optional `UTCDate` write.

## Links

- Issues [#287](https://github.com/open-astro/AlpacaBridge/issues/287), [#301](https://github.com/open-astro/AlpacaBridge/issues/301), [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 2), [#1472](https://github.com/open-astro/AlpacaBridge/issues/1472); PRs [#291](https://github.com/open-astro/AlpacaBridge/pull/291), [#351](https://github.com/open-astro/AlpacaBridge/pull/351).
- Implementation: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` (`utc_now_locked()`, `client_utc_now_locked()`).
- Regression tests: `AlpacaCore/tests/test_skywatcher_async.cpp` cases for the #287/#301 branches, the NTP-disciplined host, UTCDate readback, and an external clock step.
