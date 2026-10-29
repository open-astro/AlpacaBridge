# Hold window

Status: accepted

## Context

Each telescope driver except the Celestron CGX one keeps a "Slewing stays true for N s after a goto" deadline and read `window still open OR the mount reports slewing`. Most telescope and camera drivers keep an "IsPulseGuiding stays true until the pulse ends" deadline in the same shape. Each is a raw `time_point` field with its own arm and clear sites, and several sit on `std::chrono::steady_clock` while the driver's waits run on the task clock ([decision 0005](0005-task-clock.md)). The window was where the bugs lived: #580 (a sync 10.1" short) came from landing detection that waited out a fixed 8 s plus 3 s window instead of polling the board, and #715 replaced that wait with a poll of `:f` that ignores the window. ZWO #720, #1704 and #1724 and SynScan #880 were arm or clear mistakes in the same field.

The window exists for one reason: a client that polls `Slewing` between the dispatch of `SlewToCoordinatesAsync` and the mount's first status reply must read true, as the ASCOM `Slewing` definition requires once the call has returned. The mount, not the window, says when the slew ended.

## Decision

- **One clock-free value type, `util::HoldWindow`** (`AlpacaCore/include/alpacacore/util/hold_window.h`): `arm(now, hold)`, `active(now)` (true while `now < until`) and `clear()`. The caller passes `now` from its `TaskClock`, the same form as `util::StreamLinkHealth`; the driver's state lock guards the window. It is copyable, holds no clock reference and is not a base class.
- **Semantics every adopter follows.**
  - Arm at dispatch, before the command goes out.
  - `arm()` sets `until = now + hold` unconditionally. A later, shorter arm shortens an open window.
  - A client-facing read is `window.active(now) || <the device reports it>`. The window never masks a device that reports slewing.
  - `clear()` on every exit path: abort, failure to dispatch, cancel, connect, disconnect, and landing, because ASCOM `Slewing` is false once the slew has ended.
  - Landing detection never waits on the window. It polls the device ([decision 0005](0005-task-clock.md), #715).
  - The hold length stays a per-driver literal passed to `arm()`.
- **Settle is not in the type.** The settle after landing is a wait in a task body (`clock_.sleep_for`); the window is a read predicate. A type that both slept and answered would need a clock reference and a lock discipline, and would mix a wait with a client contract. The type has no wait method, so it cannot become the landing wait of #580.
- **The pulse-guide deadline is the second use.** A per-axis driver holds `std::array<util::HoldWindow, 2>`. The hold is the pulse duration plus the driver's completion delay. ASCOM `IsPulseGuiding` is true while a pulse is in progress; the deadline is the ownership backstop when the pulse task's own end is the primary signal (Sky-Watcher, #559).
- **Sky-Watcher adopts it first.** `slew_force_until_` and `pulse_guide_end_time_` become `HoldWindow`s. Hold lengths do not change.
- **The other telescope drivers follow, one issue each**; the Adoption table below is the roster. The cameras' pulse deadlines are not in this roster: they move in their capture-session slices ([decision 0020](0020-capture-session.md)).

## Rules

The rules a change is checked against.

1. A telescope driver's "Slewing stays true after a goto" deadline is a `util::HoldWindow`, not a raw `time_point`. Scope: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_hold_window.cpp`, `AlpacaCore/tests/test_skywatcher_async.cpp` ("a short goto reports landed before the 8 s hold ends").
2. A hold window is armed before the command is sent and cleared on abort, a failed dispatch, a cancel, connect, disconnect and landing. Scope: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_async.cpp` ("Slewing is true as soon as SlewToCoordinatesAsync returns", "AbortSlew inside the hold clears Slewing at once", "a reconnect inside the hold leaves Slewing false", "a short goto reports landed before the 8 s hold ends"), review (a failed-dispatch or cancel path that returns with the window still armed).
3. Landing detection polls the device and never reads a hold window. Scope: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_async.cpp` ("a short goto reports landed before the 8 s hold ends"), `AlpacaCore/tests/test_skywatcher_pointing.cpp` ("landing is reported when the board reports stopped, not at the 8 s floor").
4. A pulse-guide deadline is a `util::HoldWindow` on the driver's task clock. Scope: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_async.cpp` ("a pulse keeps its axis while virtual time has not reached its end (#994)").
5. `util::HoldWindow` holds no clock reference and has no method that waits or sleeps; it stays a value type, not a base class. Scope: `AlpacaCore/include/alpacacore/util/hold_window.h`. Checked by: `AlpacaCore/tests/test_hold_window.cpp`, review (a `TaskClock` member, a wait or sleep method, or a virtual member in the header).

## Adoption

The roster of telescope drivers whose slew and pulse deadlines are `util::HoldWindow`s. `scripts/check_docs_drift.py` check 23 holds it to the code.

Scan: `AlpacaCore/src/vendors/*/*telescope_driver.cpp`
Symbol: `HoldWindow`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Sky-Watcher telescope | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #1798 | pilot (#1815); slew window and per-axis pulse windows |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | planned | #1831 | |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | planned | #1832 | slew window only |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | planned | #1833 | |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | planned | #1834 | |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | planned | #1835 | |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | planned | #1836 | |

## Alternatives rejected

- **Keep seven copies.** Every window bug above was an arm or clear mistake in one copy.
- **A base class or mixin `TimedSlewDriver`.** A base class per helper is the shape [decision 0013](0013-designs-to-avoid.md) warns against.
- **A type that holds `TaskClock&` and offers `wait_until_closed()`.** A waitable window is the landing wait that produced #580.
- **Fold the settle into the type.** A wait and a predicate are different contracts (above).
- **Separate `SlewWindow` and `PulseWindow` types.** The bodies are identical; the per-axis case is an array.
- **Normalise the hold lengths (8 s, 5 s, 2 s) now.** Each is a vendor-observed value, and changing one is a behaviour change that needs a rig.

## Consequences

- A driver that adopts the type cannot compare a window against the wrong clock: the only clock it sees is the `now` it passes.
- The Rules cover every telescope driver. A driver that still keeps a raw deadline, or does not clear its window on landing, is a defect: a `planned` or `partial` row naming its issue. A migration PR changes the driver and its row, not the Rules.

## How to reverse

Delete the header and its test file and restore the two Sky-Watcher fields from the commit before this record. No other driver depends on the header until its follow-up. Mark this record `Status: retired (#NNNN)`, or `superseded by NNNN` when a successor record replaces it. Hold lengths are unchanged, so no user-visible timing changes to undo.

## Links

- Issues #1798 (the pilot) and #1831 to #1836 (the migrations); #580, #715, #559, #994, #720, #1704, #1724, #880.
- [Task clock](0005-task-clock.md), [Getters never wait on device I/O](0016-getters-never-wait-on-device-io.md).
- ASCOM `ITelescopeV4`: `Slewing`, `IsPulseGuiding`.
