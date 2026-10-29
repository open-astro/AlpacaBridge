# Mount UTC offset

Status: accepted

## Context

Several telescope drivers keep the same anchor pair for UTCDate readback: the UTC instant a client wrote, the `steady_clock` time of the write, and a valid flag. The readback is the written value plus the steady time elapsed since. Celestron and SynScan carried a comment saying this runs on the real steady clock on purpose, because [decision 0005](0005-task-clock.md) says the task clock is "never for LST or UTCDate". The readback could not be tested without a real sleep, and 0005 forbids a sleeping test on a driver that is on the clock. Celestron, SynScan and OnStep cleared the value on connect only, iOptron on connect and disconnect, ZWO never. Sky-Watcher's offset model, with host-discipline policy and clock-step detection, is [decision 0001](0001-skywatcher-pointing-clock.md) and does not move.

## Decision

1. **The split.** The UTC value stays on `std::chrono::system_clock`: it is what the client, the mount or the host said, and no task clock produces it. The host time elapsed since the write is a monotonic duration, measured between two `TaskClock::clock::time_point` values the driver takes from its clock. In production that is `steady_clock::now()`, so the readback is unchanged; in a test `FakeTaskClock::advance()` moves it.
2. **A clock-free value type.** `util::UtcOffset` (`AlpacaCore/include/alpacacore/util/utc_offset.h`) has `set(utc, now)`, `current(now)`, `valid()` and `clear()`. It holds no clock; every method that needs the time takes `now`, the shape of `util::StreamLinkHealth`. It is copyable, has no virtuals, is not a base class, and is guarded by the driver's `mutex_`. Overflow of the duration cast in `current()` is not guarded.
3. **Lifetime.** The stored value lives until the next connect or disconnect; a driver calls `clear()` on both. Each driver that adopts the type states in its PR what changed for it. Celestron changes from clearing on connect only to connect and disconnect.
4. **Seeding stays in the driver.** Reading the mount clock on the first `get_utc_date`, the connect-time host sync and `util::ClientUtcWarning::warn_once` call `set()` or stay as they are; the type does not know where a value came from.
5. **Adoption.** Celestron was the pilot. SynScan, iOptron, OnStep and the ZWO mount followed, each passing `clock_.now()` from the task clock ([decision 0005](0005-task-clock.md)); the Adoption table is the roster. Sky-Watcher and Celestron CGX are out: both keep the [decision 0001](0001-skywatcher-pointing-clock.md) host-clock offset model (step detection through `util::HostClock`), which the Alternatives section rejects for `UtcOffset`.

This refines one sentence of 0005. "Never for LST or UTCDate" means the value: LST and the UTC a client wrote stay off the task clock. The elapsed time since a UTCDate write is a deadline-style duration and reads the task clock through `UtcOffset`. 0001 is not changed.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A telescope driver not on the [decision 0001](0001-skywatcher-pointing-clock.md) host-clock offset model keeps its UTCDate value on `std::chrono::system_clock` and measures only the elapsed time since a write, through `util::UtcOffset` with `now` from the driver's clock. Scope: `AlpacaCore/src/vendors/*/*_telescope_driver.cpp`, `AlpacaCore/include/alpacacore/util/utc_offset.h`. Checked by: `AlpacaCore/tests/test_utc_offset.cpp`, review.
2. `util::UtcOffset` holds no clock, has no virtuals, is not a base class and is guarded by the driver `mutex_`. Scope: `AlpacaCore/include/alpacacore/util/utc_offset.h`. Checked by: review.
3. An adopter clears the stored value on connect and on disconnect. Scope: `AlpacaCore/src/vendors/*/*_telescope_driver.cpp`. Checked by: review.

## Alternatives rejected

- **Keep every anchor on raw `steady_clock`.** The readback cannot be tested without a real sleep, and each clock ticket would reopen the split.
- **Put the UTC value on the task clock.** A fake clock starts at the steady epoch and has no relation to UTC; it is what 0005 forbids and would reopen 0001.
- **Hold a `TaskClock&` inside the type.** It makes the type non-copyable and forces every adopter onto the clock first.
- **Use the Sky-Watcher offset model with step detection for all drivers.** Step detection compares system elapsed with steady elapsed; on a fake clock the two always disagree, so every test advance would read as a clock step.
- **A base class holding the UTC fields.** A base class per helper is rejected in [decision 0013](0013-designs-to-avoid.md).

## Consequences

One header, one test file and this record. Celestron's `last_utc_set_`, `last_utc_set_monotonic_` and `last_utc_valid_` become one `UtcOffset`, read with `clock_.now()`. The other adopters made the same change in their own PRs.

## How to reverse

Revert the Celestron change to restore three fields from the commit before it, delete the header and its test, and set this record to `Status: superseded by NNNN`. No config, protocol or on-disk format is touched.

## Adoption

`scripts/check_docs_drift.py` check 23 holds this roster to the code.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `UtcOffset`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #1385 | pilot |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted | #1409 | TaskClock elapsed time; clears on connect and now on disconnect too |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | adopted | #1590 | `utc_offset_` set and read with `clock_.now()` |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1378 | with its clock ticket; clears on disconnect too |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted | #1522 | TaskClock elapsed time; clears on fresh connect, disconnect and failed initialization |

## Links

- Upstream issue [#1385](https://github.com/open-astro/AlpacaBridge/issues/1385).
- [Decision 0001](0001-skywatcher-pointing-clock.md) (pointing time), [decision 0005](0005-task-clock.md) (task clock).
- `AlpacaCore/include/alpacacore/util/link_health.h` (the `now`-parameter shape).
