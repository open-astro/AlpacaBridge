# Getters never wait on device I/O

Status: accepted

## Context

Several drivers hold the driver `mutex_` across a device transaction: an SDK call, a serial exchange, a connect handshake or a filter-wheel move. Every getter that takes the same `mutex_` then waits behind the wire, even a getter that needs no device access (`Name`, `UniqueID`, `MaxStep`, a names list). The HTTP thread serving it stalls for the length of the transaction, and a client that polls `Connected` or `Name` during a connect stalls the request thread that serves it for the length of the connect. Examples found on `upstream/main`: the ZWO EAF and CAA `Name`, `UniqueID` and `MaxStep` take `mutex_`, which `set_connected(true)` holds across the SDK open; the ToupTek focuser `read_status()` holds `mutex_` across the status refill, and the ToupTek thermal switch holds it across every heater and fan read. The older rule in the instructions ("hold `mutex_` across the whole SDK call", shape (a)) is the pattern that produces this. Reviewers check code against those files, so the rule changes first. The migration was [issue #295](https://github.com/open-astro/AlpacaBridge/issues/295) (now closed), sliced one driver family per PR; this record is slice 1 ([#1390](https://github.com/open-astro/AlpacaBridge/issues/1390)).

## Decision

1. **A member that needs no device access never waits on one that does.** No getter takes a lock that is held across a device transaction.
2. **One I/O lock per link.** Device transactions run under one per-link lock: an `io_mutex_`, or the lock of the `TtlStatusCache` or protocol wrapper that already serialises the wire. The `util::StateSnapshot` mutex is not an I/O lock: [decision 0010](0010-snapshot-served-state.md) holds it only to copy a frame in or out, never across device I/O. A getter served from a snapshot under 0010 already meets rule 1; this record covers what 0010 does not: identity, limits and `Can*`, `Connected`, and drivers with no snapshot. The driver `mutex_` is a short state lock and is never held across I/O, outside the camera exception (item 6).
3. **Identity and limits are published at the end of connect.** `Name`, `Max*`, `Can*` and names lists are published once (the `UniqueID` is the router stored ID, a minted GUID or a kept ID, assigned before registration since [decision 0018](0018-persisted-unique-id.md)), as atomics for single scalars or under the short state lock for multi-field state. `Connected` is lock-free (an atomic).
4. **The connect handshake runs under `transition_mutex_`**, which no getter takes. In a converted driver, `set_connected(false)` clears state under `mutex_` and releases it, then takes the I/O lock across the close, so an exchange in flight never sees a closed handle and the I/O lock is never taken under `mutex_`. This holds for cameras too: a camera's connect is a handshake, not a short SDK call, so it never runs under `mutex_` (item 6). The `firmware_mutex_` cache stays a dedicated narrow mutex, as before.
5. **Lock order in a converted driver: `transition_mutex_` → I/O lock → state `mutex_` (leaf of the driver's own locks) → SDK-wrapper mutex.** The state lock is the leaf: nothing that waits on the wire is taken under it. A driver class converts in one PR, so the old and the new order never mix in one class. A camera's operational calls keep `mutex_` → operation lock → SDK-wrapper mutex (item 6); a driver that still holds `mutex_` across I/O is a `partial` row with its issue. A helper's mutex may nest under `mutex_` only as a leaf: it takes no other lock and runs no I/O or joins. The `util::AsyncOperation` slot mutex is one, as [decision 0006](0006-async-operation-ownership.md) sets, and the `StateSnapshot` mutex is another (0010 adopters publish and `write()` under `mutex_`); the driver lifecycle instructions list the shared leaves and each vendor file its own.
6. **Camera operations.** `with_camera` and `with_handle` around short SDK calls stay allowed for a camera's operations. Connect and disconnect are not operations: they follow item 4. An exposure is not a blocking transaction under `mutex_`. A camera moves its operational calls to the item 5 order only after a measurement shows a getter stall.
7. **Defects in snapshot adopters.** [Decision 0010](0010-snapshot-served-state.md) records two paths that held `mutex_` across a device exchange: Celestron's FindHome completion check (and the up-to-three stop exchanges, then every other exchange of the driver; fixed in #1437), and Bisque's control-path ends (sync, slew complete, FindHome, Park) that sampled under `mutex_`, then every other exchange of the driver; fixed in #1438. Both broke rule 1, and this record allows neither. Both are fixed, in #1437 (Celestron) and #1438 (Bisque); the I/O lock serialises a safety stop, so it needs no `mutex_`.
8. **A wire call that releases `mutex_` leaves no stale state and no untracked lock.** In a converted driver the wire call drops the state lock for the exchange and takes it back after, so a `*_locked` helper that reaches the wire releases `mutex_` partway through. State read before the exchange is re-read after it, or checked against a counter that every change bumps. A per-thread lock tracker that cannot record a lock throws `DriverException` before it takes any lock: a `mutex_` taken untracked is never dropped by the wire call, so every exchange under it breaks item 1.

The how-to and its history stay in the [driver lifecycle instructions](../../.github/instructions/driver-lifecycle.instructions.md).

## Rules

1. A getter that needs no device access takes no lock that is held across a device transaction, and the driver `mutex_` is not held across an SDK call, serial exchange, connect handshake or move; a camera's short `with_camera`/`with_handle` SDK call is bounded by Rule 4. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (trace getter locks and every device exchange in the converted class).
2. Lock order in a converted driver is `transition_mutex_`, then the I/O lock, then the state `mutex_` (leaf), then the SDK-wrapper mutex; one driver class never mixes it with the old order, and a helper mutex nests under `mutex_` only as a leaf that takes no other lock and does no I/O or joins (the driver lifecycle instructions list the shared leaves and each vendor file its own). Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `.github/instructions/driver-lifecycle.instructions.md`. Checked by: review (inspect nested acquisitions and confirm each leaf's call graph contains no other locks, I/O or joins).
3. `Connected` is lock-free, and identity and limits are published at the end of connect. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (Connected reads an atomic without waiting; getters use connect-published metadata).
4. A camera driver's `with_camera` and `with_handle` hold `mutex_` across short SDK calls, never across an image wait, an exposure or the connect handshake, and a camera moves its operational calls to the order of Rule 2 only with a measured getter stall. Scope: `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: review (an image wait, exposure or connect under `mutex_`; an operational lock-order conversion that cites no measured stall).
5. `AGENTS.md` and the driver lifecycle instructions state this rule and link this record. Scope: `AGENTS.md`, `.github/instructions/driver-lifecycle.instructions.md`. Checked by: review (both instruction owners retain the lock-order rule and a link to this record).
6. In a converted driver, state read before a wire call that releases `mutex_` is re-read or re-validated after it, and a per-thread lock tracker that cannot record a lock throws `DriverException` before it takes any lock (item 8). Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review (a value read before a wire call and used after it with no re-read or counter check; a lock-table-full path that takes `mutex_` or only logs).

## Adoption

The roster of drivers checked against the Rules above, from a full audit on 2026-10-29. A `partial` row names the issue that fixes it. `scripts/check_docs_drift.py` check 23 holds it to the code; a Symbol cell names the lock, or the class it delegates to, of a driver that has no `transition_mutex_`.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `transition_mutex_`

| Driver | Symbol | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- | --- |
| Altair camera | `create_toupcam_family_camera` | `AlpacaCore/src/vendors/altair/altair_camera_driver.cpp` | partial | #1844 | runs the ToupTek camera class |
| Astroasis focuser |  | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted |  | served from `PolledState` |
| Bisque telescope |  | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | partial | #1838 | rule 6: slew gate re-locks after a live `is_tracking()` without re-checking park |
| Celestron CGX telescope |  | `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp` | partial | #1837 | coordinate getters and Sync hold `mutex_` across AUX reads |
| Celestron focuser |  | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted |  |  |
| Celestron telescope |  | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted |  |  |
| Gemini flat panel |  | `AlpacaCore/src/vendors/gemini/gemini_flatpanel_driver.cpp` | adopted |  |  |
| Gemini focuser |  | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | partial | #1840 | rule 3: MaxStep is a live `:08#` read |
| Gemini PDH switch |  | `AlpacaCore/src/vendors/gemini/gemini_pdh_switch_driver.cpp` | adopted |  |  |
| gphoto camera | `exposure_lifecycle_mutex_` | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | partial | #1845 | `mutex_` held across the SDK connect |
| iOptron filter wheel |  | `AlpacaCore/src/vendors/ioptron/ioptron_filterwheel_driver.cpp` | adopted |  |  |
| iOptron iEAF focuser |  | `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_focuser_driver.cpp` | adopted |  |  |
| iOptron iGuider camera |  | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | adopted |  |  |
| iOptron switch |  | `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp` | adopted |  |  |
| iOptron telescope |  | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | partial | #1829 | `mutex_` held across connect, getter refills and commands |
| OnStep telescope | | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | | `util::LockGate` drops `mutex_` per exchange |
| Player One camera |  | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | partial | #1841 | `mutex_` held across the SDK connect |
| Player One filter wheel |  | `AlpacaCore/src/vendors/playerone/playerone_filterwheel_driver.cpp` | adopted |  |  |
| Player One thermal switch | `mutex_` | `AlpacaCore/src/vendors/playerone/playerone_switch_driver.cpp` | partial | #1839 | no transition or I/O lock; `mutex_` held across the SDK connect |
| QHY camera |  | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | adopted |  | on the I/O-lock order |
| QHY CFW3 filter wheel |  | `AlpacaCore/src/vendors/qhy/qhy_cfw3_filterwheel_driver.cpp` | adopted |  |  |
| QHY filter wheel |  | `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp` | adopted |  |  |
| QHY focuser |  | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted |  | served from `PolledState` |
| Sky-Watcher telescope |  | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted |  | `util::LockGate` drops `mutex_` per exchange |
| SVBONY camera |  | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | partial | #1842 | `mutex_` held across the SDK connect |
| SynScan telescope | `mutex_` | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | partial | #1828 | `mutex_` held across the connect handshake |
| ToupTek camera | `exposure_lifecycle_mutex_` | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | partial | #1844 | `mutex_` held across the SDK connect |
| ToupTek filter wheel |  | `AlpacaCore/src/vendors/touptek/touptek_filterwheel_driver.cpp` | adopted |  |  |
| ToupTek focuser |  | `AlpacaCore/src/vendors/touptek/touptek_focuser_driver.cpp` | adopted |  |  |
| ToupTek switch |  | `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp` | adopted |  |  |
| ToupTek thermal switch |  | `AlpacaCore/src/vendors/touptek/touptek_thermal_switch_driver.cpp` | adopted |  |  |
| WandererAstro box switch |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_box_switch_driver.cpp` | adopted |  |  |
| WandererAstro cover |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_covercalibrator_driver.cpp` | adopted |  | writes with no reply wait under `state_mutex_` (Alternatives rejected) |
| WandererAstro filter wheel |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_driver.cpp` | adopted |  | writes with no reply wait under `state_mutex_` (Alternatives rejected) |
| WandererAstro rotator |  | `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_driver.cpp` | adopted |  |  |
| WeeWX observing conditions |  | `AlpacaCore/src/vendors/weewx/weewx_observingconditions_driver.cpp` | adopted |  |  |
| ZWO ASIAIR Plus switch |  | `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp` | adopted |  |  |
| ZWO ASIAIR switch |  | `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp` | adopted |  |  |
| ZWO camera |  | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | partial | #1843 | `mutex_` held across the SDK connect |
| ZWO filter wheel |  | `AlpacaCore/src/vendors/zwo/zwo_filterwheel_driver.cpp` | adopted |  |  |
| ZWO focuser |  | `AlpacaCore/src/vendors/zwo/zwo_focuser_driver.cpp` | adopted |  |  |
| ZWO rotator |  | `AlpacaCore/src/vendors/zwo/zwo_rotator_driver.cpp` | adopted |  |  |
| ZWO switch |  | `AlpacaCore/src/vendors/zwo/zwo_switch_driver.cpp` | adopted |  |  |
| ZWO telescope | `set_connected_mutex_` | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted |  | transition lock named `set_connected_mutex_` |

## Alternatives rejected

- **The #295 proposal's order `mutex_` → `io_mutex_`.** A transaction that holds `mutex_` while it waits for `io_mutex_` still parks every `mutex_` getter behind the wire. The state lock must be the leaf.
- **Atomics for all state.** Multi-field state (names lists, limits) would tear. Atomics are for `connected_` and single scalars only.
- **Convert the cameras' operations now.** `with_camera` and `with_handle` hold `mutex_` across short SDK calls by design, and no getter stall has been measured. Out of scope until one is (evidence over narrative).
- **Wanderer cover and filter-wheel `state_mutex_`, and the QHY CFW3 `Position` fallback.** Fire-and-forget writes with no reply wait, and a path that runs only after a failed move. No measurable stall.

## Consequences

- Reviewers check each driver PR against the Rules above. A driver that still holds `mutex_` across device I/O is a defect with its own issue, not an exception.
- Each slice is one driver family. A getter that must reach the device (a live read) still takes the I/O lock and waits; the rule covers members that need no device access.
- #295 closed with drivers still unconverted. The Adoption table above is the roster, and each `partial` row names its issue (SynScan #1828, iOptron #1829, the cameras #1841 to #1845, and others).

## How to reverse

Each slice is one driver family in one PR; reverting that PR restores the old locking for that family alone. To drop the rule, supersede this record and restore shape (a) in the driver lifecycle instructions and the `AGENTS.md` line.

## Links

- Issues [#295](https://github.com/open-astro/AlpacaBridge/issues/295) (the migration), [#1390](https://github.com/open-astro/AlpacaBridge/issues/1390) (this slice), [#1449](https://github.com/open-astro/AlpacaBridge/issues/1449) (OnStep: a getter whose 2 s cache is stale takes the I/O lock for that one read), [#1729](https://github.com/open-astro/AlpacaBridge/issues/1729) and [#1730](https://github.com/open-astro/AlpacaBridge/issues/1730) (item 8: the shared lock gate and the Sky-Watcher full-table defect).
- Rules: [driver lifecycle instructions](../../.github/instructions/driver-lifecycle.instructions.md).
- Related: [decision 0010](0010-snapshot-served-state.md) (snapshot-served state), [decision 0006](0006-async-operation-ownership.md) (async operation ownership; the slot mutex nests under `mutex_`).
