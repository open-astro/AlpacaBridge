# Snapshot-served state

Status: accepted

## Context

A driver's getters and `DeviceState` each ask the device, or read driver fields under different locks. Request/response drivers pay a round trip per property (`TtlStatusCache` answered that for SDK-backed drivers, open-astro#294); poll-fed drivers read plain fields that a poll and a client write both change. Two defects follow: a client write can be lost to a poll that sampled before it, and a getter cannot tell a fresh value from one the link stopped refreshing.

## Decision

1. **What is served.** Getters for polled telemetry (position, tracking, slewing, status flags) read `util::StateSnapshot<T>` (`util/state_snapshot.h`) and never touch the device. Static capability and metadata answers stay outside it.
2. **One value, its age, a stale flag.** `read()` returns the frame, its `measured_at` on the task clock ([decision 0005](0005-task-clock.md)) and `stale` (age greater than `max_age`). A stale frame is still returned; the driver decides per getter whether stale means refuse (see [decision 0009](0009-link-loss-and-relink-policy.md)). Before the first publish, `read()` returns nothing.
3. **Write-through.** A client write changes the snapshot at once with `write(fn)`, so the next read sees it before the next poll. The write leaves `measured_at` at the last publish: a client write is not a measurement, so polled fields it did not touch must not read as fresh. A write with no frame held (before the first publish, after `reset()`) is not retained and `read()` keeps returning nothing, but it still drops a poll in flight (point 4); the next poll measures the device. `reset()` also drops a poll in flight, so a poll that sampled before disconnect cannot republish after it. The `write()` callback runs under the snapshot mutex and must not block or do device I/O.
4. **Publish-sequence guard.** A poll takes `begin_poll()` before it samples the device. Any write or `reset()` after that makes `publish()` drop the frame and return false. The next poll publishes. Chosen over comparing `measured_at` with the last write time: a counter needs no clock reading and cannot tie on a coarse or virtual clock. The cost is that a poll that overlaps a write loses its other fields too, for one poll period.
5. **Target fields.** OnStep and SynScan keep `target_ra_hours_`, `target_dec_degrees_` and the `target_*_set_` flags under `mutex_`: the setters and the getters all take it, so there is no data race today (checked against `onstep_telescope_driver.cpp` and `synscan_telescope_driver.cpp`; the review of open-astro#663 raised it, and the code does not bear it out). The rule applies when a driver's getters move onto the snapshot: its target value and flag then change together through `write()` and the getters read them from one `read()`, so the pair cannot be seen half-set and a second lock does not come back. This slice does not change the drivers.
6. **Design.** A standalone type that takes a `TaskClock`, not an extension of `TtlStatusCache`: that cache fetches inside `get()` under its lock and ties validity to a TTL and link health, while the snapshot is fed by a separate poll and must never hold its lock across device I/O. Link health stays with the existing helpers.
7. **Slice scope.** The utility, this record and fake-clock tests (`tests/test_state_snapshot.cpp`). No driver is moved in that slice; Sky-Watcher is the first (see Sky-Watcher adoption). ConformU not re-run.

### Sky-Watcher adoption

The Sky-Watcher direct driver is the first on the snapshot (open-astro#1011). These points are decided for it; accepted after the rig ConformU pass: EQM-35 Pro, full Telescope suite, 2026-10-11, 0 errors, 0 issues, 0 timing violations (`AlpacaCore/conformu/SkyWatcher/EQM-35 Pro/Linux-arm64.txt`).

1. **TimeStamp is `measured_at`.** `TelescopeDriver` gains one protected virtual, `device_state_age()`, default `nullopt`. `get_device_state()` stays `final` and inline: it reads the age once, before the getters, and stamps `TimeStamp` with the system time minus that age (`device_state_timestamp(system_clock::time_point)`). Sky-Watcher returns the task clock now minus the frame's `measured_at`. The age, not a time point, crosses the hook, so the steady task clock ([decision 0005](0005-task-clock.md)) is never mixed with wall time. Telescope drivers that have not adopted a snapshot inherit `nullopt` and their output does not change. Rejected: dropping `final` so one driver overrides `get_device_state()` (it could drift from the omit-on-throw contract the base enforces), and a virtual on `AlpacaDriver` (every device type for one driver's need). Reverse: delete the hook and the override; TimeStamp is request time again.
2. **The frame holds what the poll measured.** `PolledFrame`: the raw axis angles and the per-axis status flags (running, speed mode), nothing derived. The dead-reckoned position stays a function of the model at read time (anchor angle + commanded rate x elapsed), so the PR #221 rate-offset continuity keeps its anchor model. Commanded rates, tracking and every other client-set value stay in the driver under `mutex_`, where the setter changes them and the next getter reads them, so write-then-read holds before the next poll. What a driver write changes about the frame goes through `write()`: a rate setter's in-place re-anchor, a sync, an invalidation and the end of a slew all drop the poll in flight, and all but the re-anchor also mark the status flags as not current (`status_current`), so Slewing cannot read true from a poll that sampled before the operation ended. A direct board read on a control path publishes without dropping the poll in flight, so the model never adopts a poll sample taken before its anchor. Rejected: a frame holding the dead-reckoned position (it bakes the rate in at poll time, so a rate setter would not be seen until the next poll, and #221 continuity breaks).
3. **One poll body, owned by the driver for the connection.** An `AsyncOperation` slot started at the end of a successful connect and cancelled and joined before disconnect closes the transport, waiting on the `TaskClock`. Period 100 ms start to start, ticks missed behind a slow link are skipped, never made up. The first frame is taken at the end of connect, so the getters have an answer, status flags included, when Connect returns. The exchanges run without `mutex_` (the protocol wrapper serialises them) so a getter never waits behind a slow link; the snapshot mutex is never held across I/O (point 3 above). The poll takes `mutex_` only around its exchanges, never across one: before them, to check that its connection is still open and read the counts per revolution, and after them, to publish and re-anchor the model. Its exchange results feed the existing link latch: with the getters off the wire it is the only silence detector while the mount is idle. Motion control (`wait_for_slew_complete`, the stop waits, the landing settle, goto planning) keeps its direct reads: they are control, not getters.
4. **Two bounds, never one.**
   - *Freshness*, `max_age` 200 ms: two poll periods, under the live limit guard's 250 ms MoveAxis poll. It drives only the guard (open-astro#436): the guard reads the snapshot while the frame is not stale and keeps its direct read while it is. A link whose four-transaction poll takes longer than 200 ms (the 100 ms-per-transaction fake) reads stale on every frame and the guard stays on its direct read; the getters still serve the frame. Stale is never a refusal.
   - *Lost*, [decision 0009](0009-link-loss-and-relink-policy.md) unchanged: `util::kLinkStalenessBound` of latched fault, or transport-gone evidence at once. Lost: the getters and setters throw `NotConnected`, `Connected` reads false, `DeviceState` is the empty list, `link_lost_at` is stamped (serial and UDP), the poll body ends and the frame is dropped. A frame older than the bound with no latch (a poll that stopped answering the clock) is lost too, but nothing ends its poll body or drops it then: a Connect relinks, stamps `link_lost_at` at the moment the frame crossed the bound, and drops the frame. Faulted (latched, younger than the bound): the getters throw the `DriverException` with the fault text, `Connected` stays true.
   - Rejected: a 200 ms `NotConnected` bound (decision 0009 alternative B: it drops `Connected` on any Wi-Fi hiccup) and a single bound for both (it cannot be both under 250 ms and at least twice the link latency). Reverse: revert the slice; the getters return to per-call reads.

#### Celestron telescope adoption (#1337)

- One status poll per connection, 1 s start to start, fills `StateSnapshot<PolledFrame>`: equatorial pair, slewing flag, and every fourth tick alt-az, pier side and tracking mode. The sample and site exchanges run without the driver `mutex_` and the frame is published under it. The FindHome completion check runs under `mutex_`, so on a dead link during FindHome the getters wait behind it for one exchange timeout per tick. That is a defect under [decision 0016](0016-getters-never-wait-on-device-io.md) rule 1, tracked in #1437 (the Celestron slice of #295). `kSnapshotMaxAge` is 3 s and stale is served, never refused.
- Served from the frame: RightAscension, Declination, Altitude, Azimuth, Slewing, SideOfPier, Tracking, AtHome while homing, DeviceState (`TimeStamp` is `measured_at`). The 2 s position cache and its stamps are gone. Control-path reads (SetPark, Park, the slew loops) measure directly and write through.
- Slewing carries a `slewing_known` flag: a connect whose slewing query failed serves "not available yet" until a poll answers, not a guessed `false`.
- The site read the poll does for a mount whose connect-time read failed (retried every 30 s, so a link whose location never reads still samples on most full ticks) stores its result only while no client write has made the site valid.
- Lost link: [decision 0009](0009-link-loss-and-relink-policy.md) applies; a tick ends at its first failed exchange (the FindHome completion check, the location read, the Slewing and RA/Dec exchange, then each slow field), so a Disconnect waits for one exchange timeout. The tick on which the third FindHome check fails stops the mount first (up to three stop exchanges under `mutex_`), so on a dead link that tick can wait longer. Holding `mutex_` across those stops is also a defect under [decision 0016](0016-getters-never-wait-on-device-io.md) rule 1, tracked in #1437: the I/O lock serialises a stop, so it needs no `mutex_`.

#### Bisque telescope adoption (#1367)

- One status poll per connection, 1 s start to start, fills `StateSnapshot<PolledFrame>`: equatorial pair, alt-az, side of pier and Tracking. Served from the frame: RightAscension, Declination, Altitude, Azimuth, SideOfPier, Tracking, DeviceState (`TimeStamp` is `measured_at`). The 2 s position cache (`kPositionCacheTtl`) and its stamps are gone.
- Slewing stays control-path state (`slewing_cached_`): the poll asks `IsSlewComplete` only for an async slew past its 8 s force window with no `SlewToCoordinates` wait running, and clears Slewing in the same step as the landed frame.
- Control-path ends that move the mount (sync, slew complete, FindHome, Park, pulse-guide end) measure the position at once and publish it, so a read right after sees where the mount is; a pulse-guide end samples without `mutex_` (a write since drops the publish), the others sample under it, so a getter can wait behind them. Sampling under `mutex_` is a defect under [decision 0016](0016-getters-never-wait-on-device-io.md) rule 1, tracked in #1438 (the Bisque slice of #295). A slew start, an abort and a move only drop the tick in flight.
- The slew Tracking gate reads TheSkyX live, writes the result through, and falls back to the frame's value when that read fails. When the frame never knew Tracking, the gate treats it as off, where the Tracking getter throws.
- Lost link: [decision 0009](0009-link-loss-and-relink-policy.md) applies.


## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. In a driver whose Adoption row is adopted, getters for polled telemetry read the `util::StateSnapshot` and never touch the device. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review; the roster by `scripts/check_docs_drift.py` check 23.
2. A client write goes through `write()` and leaves `measured_at` at the last publish. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/util/state_snapshot.h`. Checked by: `AlpacaCore/tests/test_state_snapshot.cpp`.
3. A poll takes `begin_poll()` before it samples, and a write or `reset()` after that drops its publish. Scope: `AlpacaCore/include/alpacacore/util/state_snapshot.h`, `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: `AlpacaCore/tests/test_state_snapshot.cpp`.
4. The snapshot mutex is never held across device I/O, and a `write()` callback does no I/O. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review.
5. A telescope that serves `DeviceState` from the snapshot stamps `TimeStamp` with `measured_at` through `device_state_age()`. Scope: `AlpacaCore/include/alpacacore/telescope_driver.h`, `AlpacaCore/src/vendors/*/*_telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_snapshot.cpp`, review.
6. Each adopter states per getter what a stale frame means, in its instruction file; stale alone never refuses. Scope: `.github/instructions/*.instructions.md`. Checked by: `AlpacaCore/tests/test_contract_sweep.cpp` (the "Contract sweep snapshot" cases: a stale frame is served, never refused, for every adopted driver), `scripts/check_contract_sweep.py` (rule 6: the sweep roster matches the Adoption table), review (the per-getter statement in the instruction file).

## Alternatives rejected

- **Extend `TtlStatusCache`.** Different ownership of the fetch (see point 6).
- **Compare `measured_at` with the last write time for the guard.** Ties on equal clock readings (point 4).
- **A per-field sequence.** More state for a poll period of lost fields.

## How to reverse

Delete the header and its test. No driver depends on it until a later slice moves one; reversing then means restoring that driver's field reads.

## Consequences

- Getters for polled telemetry answer from memory and carry their age; each driver that moves onto the snapshot states per getter what stale means.
- A client that writes at least once per poll period keeps every poll's frame dropped, so the frame ages and reads stale; that is intended, because the device was not measured.
- A write before the first publish is lost (point 3); drivers that need it set it through the device or hold it outside the snapshot.

## Adoption

The roster of drivers that serve polled telemetry from a snapshot. The sections above record the Sky-Watcher, Celestron and Bisque choices; each other driver's per-getter choices are owned by its instruction file. `scripts/check_docs_drift.py` check 23 holds the roster to the code.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `StateSnapshot`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Sky-Watcher telescope | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #1011 | 100 ms; Sky-Watcher adoption above |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #1337 | 1 s; Celestron telescope adoption above |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1367 | 1 s; Bisque telescope adoption above |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted | #1247 | 1 s; [SynScan instructions](../../.github/instructions/synscan.instructions.md) |
| Gemini focuser | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | adopted | #1229 | 250 ms; [Gemini instructions](../../.github/instructions/gemini.instructions.md) |
| QHY Q-Focuser | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted | #1310 | 250 ms; [QHY instructions](../../.github/instructions/qhy.instructions.md) |
| Astroasis focuser | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted | #1327 | 100 ms; [Astroasis instructions](../../.github/instructions/astroasis.instructions.md) |
| Celestron focus motor | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted | #1320 | 250 ms; [Celestron instructions](../../.github/instructions/celestron.instructions.md) |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | planned | — | |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | planned | — | |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted | #1432, #1433 | 250 ms after each poll; 5 s freshness threshold; mandatory GU+RA/Dec sample age is independent of partial publication; [ZWO instructions](../../.github/instructions/zwo.instructions.md) |

## Links

- [Task clock](0005-task-clock.md)
- [Link-loss and relink policy](0009-link-loss-and-relink-policy.md)
- `AlpacaCore/include/alpacacore/util/state_snapshot.h`, `AlpacaCore/tests/test_state_snapshot.cpp`, `AlpacaCore/tests/test_skywatcher_snapshot.cpp`
