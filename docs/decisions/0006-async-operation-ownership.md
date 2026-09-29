# Async operation ownership

Status: proposed

## Context

The Sky-Watcher, Celestron and SynScan telescope drivers each hand-roll the same background-task machinery. `task_wait_for` (in `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`, `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` and `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp`) is a condition-variable wait on `task_mutex_` with a cancel-flag predicate. `reap_slew_task`, in all three files, sets the cancel flag, notifies, moves the thread out under `task_mutex_` and joins it with no bound. Every new slew, park or home calls it on the HTTP thread, so a new command waits for the whole of the old body, including a serial transaction that is blocked until its own timeout. Sky-Watcher also has a per-axis `reap_stop_task(int axis)` and a `MoveAxis(axis, 0)` stop-completion body that checks the driver-wide `motion_generation_` at its tail, so that a slew, park, home or pulse that took over while it polled makes it skip its tracking restore. That check is the only place in the tree that tells a cancelled body from a superseded one, the distinction [CONTEXT.md](../../CONTEXT.md) names.

## Decision

A driver's cancellable background bodies run in `util::AsyncOperation` slots (`AlpacaCore/include/alpacacore/util/async_operation.h`), one body at a time per slot, and every wait in a slot goes through the injected `TaskClock` of [decision 0005](0005-task-clock.md).

`start()` does its capacity check, the move of the current body into the stale list, the generation bump, the thread creation and the assignment of the new body under the slot's one mutex, with no `joinable()` pre-check outside it. It does not wait for the body it replaces: it marks it superseded, wakes it and moves it to the stale list, joining only stale bodies that have already returned. At most three replaced bodies may still be running; a start that would make that four first waits on the clock for a stale body to return, for at most 2 s (the bounded-join budget, under the roughly 5 s an ASCOM client allows a call), and otherwise refuses with `InvalidOperation` and changes nothing: the current body is not superseded, the generation is not bumped and the kept failure is not cleared. That is the only wait `start()` makes.

Every start bumps an `OperationGeneration` that several slots may share (a slew slot and per-axis stop slots), and a driver may bump it directly for a motion command that runs without a slot. Which slots share one is the driver's choice; the slot knows nothing about axes. A body's stop reason is superseded when it was replaced on its own slot or the shared generation has moved past its token, otherwise cancelled when `cancel()` or `cancel_all_and_join()` reached it, otherwise none. Superseded takes precedence over cancelled whenever both apply, and the reason is sticky.

A body's `wait_for(d)` waits on the slot mutex and condition variable through the clock and returns whether the body may continue, evaluated under that mutex, so a cancel at the deadline wins. A cancel or a replacement on the body's own slot wakes it at once; a generation bump elsewhere does not, and the body sees it when its wait returns. `cancel()` marks and wakes the current body, never joins and never blocks. `cancel_all_and_join()` and the destructor cancel the current body, wake every body and join every thread with no time bound; the slot never detaches a thread, and bounding a body that blocks in a call with no timeout of its own stays the caller's job. A throw from the body the slot started last is kept as the last failure until the next successful start, including when `cancel_all_and_join()` took that body and no start followed. A throw from a stale body, one replaced by a later start or taken by `cancel_all_and_join()` before a later start, is logged with `ALPACA_LOG_WARN` and never written to the last failure, so it cannot overwrite what the later start cleared, whatever order the throw and the start happen in. No throw reaches `std::terminate`.

Lock order is driver `mutex_`, then the slot mutex, and the slot runs no body, log call or user callback under its mutex. `start()` and `cancel_all_and_join()` are called without the driver mutex held, since a stale body may need it to return; `cancel()`, `running()`, `stale_count()` and `last_failure()` may be called with it held. The slot is neither copyable nor movable.

## Alternatives rejected

Join the old body on start, today's `reap_slew_task` shape: it blocks the HTTP thread for as long as a blocked transaction lasts. Detach the old body: AGENTS.md forbids detaching a thread that touches `this`. An unbounded stale list: a hung link would grow threads without limit, one per client retry. Waking bodies across slots on a generation bump: one slot would have to take another slot's mutex, which needs a cross-slot lock order that nothing else requires. Cancelled winning over superseded: a superseded body would send a stop to axes a newer operation owns.

## Consequences

No driver uses the slot yet. The Sky-Watcher telescope is the pilot, then Celestron and SynScan; each slice deletes that driver's `task_wait_for` and `reap_*` bodies. The pilot re-checks the stale bound and the precedence rule against hardware behaviour before this record is accepted. The utility is tested on the fake clock only (`AlpacaCore/tests/test_async_operation.cpp`), with a `[stress-guard]` double-start storm under ThreadSanitizer.

## Links

- Upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584); issue [#717](https://github.com/open-astro/AlpacaBridge/issues/717); related [#535](https://github.com/open-astro/AlpacaBridge/issues/535).
- [Decision 0005](0005-task-clock.md) (task clock); [CONTEXT.md](../../CONTEXT.md) ("Cancelled vs superseded").
- Machinery this replaces: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`, `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp`, `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp`.
- Module overview in [architecture](../architecture.md#modules).
