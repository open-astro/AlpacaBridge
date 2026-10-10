# Snapshot-served state

Status: proposed

## Context

A driver's getters and `DeviceState` each ask the device, or read driver fields under different locks. Request/response drivers pay a round trip per property (`TtlStatusCache` answered that for SDK-backed drivers, open-astro#294); poll-fed drivers read plain fields that a poll and a client write both change. Two defects follow: a client write can be lost to a poll that sampled before it, and a getter cannot tell a fresh value from one the link stopped refreshing.

## Decision

1. **What is served.** Getters for polled telemetry (position, tracking, slewing, status flags) read `util::StateSnapshot<T>` (`util/state_snapshot.h`) and never touch the device. Static capability and metadata answers stay outside it.
2. **One value, its age, a stale flag.** `read()` returns the frame, its `measured_at` on the task clock ([decision 0005](0005-task-clock.md)) and `stale` (age greater than `max_age`). A stale frame is still returned; the driver decides per getter whether stale means refuse (see [decision 0009](0009-link-loss-and-relink-policy.md)). Before the first publish, `read()` returns nothing.
3. **Write-through.** A client write changes the snapshot at once with `write(fn)`, so the next read sees it before the next poll. The write leaves `measured_at` at the last publish: a client write is not a measurement, so polled fields it did not touch must not read as fresh. A write with no frame held (before the first publish, after `reset()`) is not retained and `read()` keeps returning nothing, but it still drops a poll in flight (point 4); the next poll measures the device. `reset()` also drops a poll in flight, so a poll that sampled before disconnect cannot republish after it. The `write()` callback runs under the snapshot mutex and must not block or do device I/O.
4. **Publish-sequence guard.** A poll takes `begin_poll()` before it samples the device. Any write or `reset()` after that makes `publish()` drop the frame and return false. The next poll publishes. Chosen over comparing `measured_at` with the last write time: a counter needs no clock reading and cannot tie on a coarse or virtual clock. The cost is that a poll that overlaps a write loses its other fields too, for one poll period.
5. **Target fields.** OnStep and SynScan keep `target_ra_hours_`, `target_dec_degrees_` and the `target_*_set_` flags under `mutex_`: the setters and the getters all take it, so there is no data race today (checked against `onstep_telescope_driver.cpp` and `synscan_telescope_driver.cpp`; the review of open-astro#663 raised it, and the code does not bear it out). The rule applies when a driver's getters move onto the snapshot: its target value and flag then change together through `write()` and the getters read them from one `read()`, so the pair cannot be seen half-set and a second lock does not come back. This slice does not change the drivers.
6. **Design.** A standalone type that takes a `TaskClock`, not an extension of `TtlStatusCache`: that cache fetches inside `get()` under its lock and ties validity to a TTL and link health, while the snapshot is fed by a separate poll and must never hold its lock across device I/O. Link health stays with the existing helpers.
7. **Slice scope.** The utility, this record and fake-clock tests (`tests/test_state_snapshot.cpp`). No driver is moved; SkyWatcher is the first (the next slice). ConformU not re-run.

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

## Links

- [Task clock](0005-task-clock.md)
- [Link-loss and relink policy](0009-link-loss-and-relink-policy.md)
- `AlpacaCore/include/alpacacore/util/state_snapshot.h`, `AlpacaCore/tests/test_state_snapshot.cpp`
