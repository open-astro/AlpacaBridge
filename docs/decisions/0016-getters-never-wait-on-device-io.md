# Getters never wait on device I/O

Status: accepted

## Context

Several drivers hold the driver `mutex_` across a device transaction: an SDK call, a serial exchange, a connect handshake or a filter-wheel move. Every getter that takes the same `mutex_` then waits behind the wire, even a getter that needs no device access (`Name`, `UniqueID`, `MaxStep`, a names list). The HTTP thread serving it stalls for the length of the transaction, and a client that polls `Connected` or `Name` during a connect stalls the request thread that serves it for the length of the connect. Examples found on `upstream/main`: the ZWO EAF and CAA `Name`, `UniqueID` and `MaxStep` take `mutex_`, which `set_connected(true)` holds across the SDK open; the ToupTek focuser `read_status()` holds `mutex_` across the status refill, and the ToupTek thermal switch holds it across every heater and fan read. The older rule in the instructions ("hold `mutex_` across the whole SDK call", shape (a)) is the pattern that produces this. Reviewers check code against those files, so the rule changes first. The migration was [issue #295](https://github.com/open-astro/AlpacaBridge/issues/295) (now closed), sliced one driver family per PR; this record is slice 1 ([#1390](https://github.com/open-astro/AlpacaBridge/issues/1390)).

## Decision

1. **A member that needs no device access never waits on one that does.** No getter takes a lock that is held across a device transaction.
2. **One I/O lock per link.** Device transactions run under one per-link lock: an `io_mutex_`, or the lock of the `TtlStatusCache` or protocol wrapper that already serialises the wire. The `util::StateSnapshot` mutex is not an I/O lock: [decision 0010](0010-snapshot-served-state.md) holds it only to copy a frame in or out, never across device I/O. A getter served from a snapshot under 0010 already meets rule 1; this record covers what 0010 does not: identity, limits and `Can*`, `Connected`, and drivers with no snapshot. The driver `mutex_` is a short state lock and is never held across I/O, outside the camera exception (item 6).
3. **Identity and limits are published at the end of connect.** `Name`, `Max*`, `Can*` and names lists are published once (the `UniqueID` is the router stored ID, a minted GUID or a kept ID, assigned before registration since [decision 0018](0018-persisted-unique-id.md)), as atomics for single scalars or under the short state lock for multi-field state. `Connected` is lock-free (an atomic).
4. **The connect handshake runs under `transition_mutex_`**, which no getter takes. In a converted driver, `set_connected(false)` clears state under `mutex_` and releases it, then takes the I/O lock across the close, so an exchange in flight never sees a closed handle and the I/O lock is never taken under `mutex_`. An unconverted driver and a camera keep their own order (item 5). The `firmware_mutex_` cache stays a dedicated narrow mutex, as before.
5. **Lock order in a converted driver: `transition_mutex_` → I/O lock → state `mutex_` (leaf of the driver's own locks) → SDK-wrapper mutex.** The state lock is the leaf: nothing that waits on the wire is taken under it. A driver class converts in one PR, so the old and the new order never mix in one class. An unconverted driver and the cameras keep `mutex_` → operation lock → SDK-wrapper mutex. A helper's mutex may nest under `mutex_` only as a leaf: it takes no other lock and runs no I/O or joins. The `util::AsyncOperation` slot mutex is one, as [decision 0006](0006-async-operation-ownership.md) sets, and the `StateSnapshot` mutex is another (0010 adopters publish and `write()` under `mutex_`); the driver lifecycle instructions list the shared leaves and each vendor file its own.
6. **Camera exception.** `with_camera` and `with_handle` around short SDK calls stay allowed. An exposure is not a blocking transaction under `mutex_`. A camera converts only after a measurement shows a getter stall.
7. **Defects in snapshot adopters.** [Decision 0010](0010-snapshot-served-state.md) records two paths that held `mutex_` across a device exchange: Celestron's FindHome completion check (and the up-to-three stop exchanges, then every other exchange of the driver; fixed in #1437), and Bisque's control-path ends (sync, slew complete, FindHome, Park) that sampled under `mutex_`, then every other exchange of the driver; fixed in #1438. Both broke rule 1, and this record allows neither. Both are fixed, in #1437 (Celestron) and #1438 (Bisque); the I/O lock serialises a safety stop, so it needs no `mutex_`.
8. **A wire call that releases `mutex_` leaves no stale state and no untracked lock.** In a converted driver the wire call drops the state lock for the exchange and takes it back after, so a `*_locked` helper that reaches the wire releases `mutex_` partway through. State read before the exchange is re-read after it, or checked against a counter that every change bumps. A per-thread lock tracker that cannot record a lock throws `DriverException` before it takes any lock: a `mutex_` taken untracked is never dropped by the wire call, so every exchange under it breaks item 1.

The how-to and its history stay in the [driver lifecycle instructions](../../.github/instructions/driver-lifecycle.instructions.md).

## Rules

1. A getter that needs no device access takes no lock that is held across a device transaction, and the driver `mutex_` is not held across an SDK call, serial exchange, connect handshake or move; a camera's short `with_camera`/`with_handle` SDK call is bounded by Rule 4. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (trace getter locks and every device exchange in the converted class).
2. Lock order in a converted driver is `transition_mutex_`, then the I/O lock, then the state `mutex_` (leaf), then the SDK-wrapper mutex; one driver class never mixes it with the old order, and a helper mutex nests under `mutex_` only as a leaf that takes no other lock and does no I/O or joins (the driver lifecycle instructions list the shared leaves and each vendor file its own). Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `.github/instructions/driver-lifecycle.instructions.md`. Checked by: review (inspect nested acquisitions and confirm each leaf's call graph contains no other locks, I/O or joins).
3. `Connected` is lock-free, and identity and limits are published at the end of connect. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (Connected reads an atomic without waiting; getters use connect-published metadata).
4. A camera driver's `with_camera` and `with_handle` hold `mutex_` across one short SDK call, never across an image wait or an exposure, and a camera converts to the order of Rule 2 only with a measured getter stall. Scope: `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: review (an image wait or exposure under `mutex_`; a lock-order conversion that cites no measured stall).
5. `AGENTS.md` and the driver lifecycle instructions state this rule and link this record. Scope: `AGENTS.md`, `.github/instructions/driver-lifecycle.instructions.md`. Checked by: review (both instruction owners retain the lock-order rule and a link to this record).
6. In a converted driver, state read before a wire call that releases `mutex_` is re-read or re-validated after it, and a per-thread lock tracker that cannot record a lock throws `DriverException` before it takes any lock (item 8). Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review (a value read before a wire call and used after it with no re-read or counter check; a lock-table-full path that takes `mutex_` or only logs).

## Alternatives rejected

- **The #295 proposal's order `mutex_` → `io_mutex_`.** A transaction that holds `mutex_` while it waits for `io_mutex_` still parks every `mutex_` getter behind the wire. The state lock must be the leaf.
- **Atomics for all state.** Multi-field state (names lists, limits) would tear. Atomics are for `connected_` and single scalars only.
- **Convert the cameras now.** `with_camera` and `with_handle` hold `mutex_` across short SDK calls by design, and no getter stall has been measured. Out of scope until one is (evidence over narrative).
- **Wanderer cover and filter-wheel `state_mutex_`, and the QHY CFW3 `Position` fallback.** Fire-and-forget writes with no reply wait, and a path that runs only after a failed move. No measurable stall.

## Consequences

- Reviewers check each driver PR against the Rules above. A driver that still holds `mutex_` across device I/O is a defect with its own issue, not an exception.
- Each slice is one driver family. A getter that must reach the device (a live read) still takes the I/O lock and waits; the rule covers members that need no device access.
- No Adoption table yet. #295 closed while the SynScan and iOptron telescopes still held `mutex_` across the connect handshake (`synscan_telescope_driver.cpp`, `ioptron_telescope_driver.cpp`); each gets its own issue, and the table is added with them.

## How to reverse

Each slice is one driver family in one PR; reverting that PR restores the old locking for that family alone. To drop the rule, supersede this record and restore shape (a) in the driver lifecycle instructions and the `AGENTS.md` line.

## Links

- Issues [#295](https://github.com/open-astro/AlpacaBridge/issues/295) (the migration), [#1390](https://github.com/open-astro/AlpacaBridge/issues/1390) (this slice), [#1449](https://github.com/open-astro/AlpacaBridge/issues/1449) (OnStep: a getter whose 2 s cache is stale takes the I/O lock for that one read), [#1729](https://github.com/open-astro/AlpacaBridge/issues/1729) and [#1730](https://github.com/open-astro/AlpacaBridge/issues/1730) (item 8: the shared lock gate and the Sky-Watcher full-table defect).
- Rules: [driver lifecycle instructions](../../.github/instructions/driver-lifecycle.instructions.md).
- Related: [decision 0010](0010-snapshot-served-state.md) (snapshot-served state), [decision 0006](0006-async-operation-ownership.md) (async operation ownership; the slot mutex nests under `mutex_`).
