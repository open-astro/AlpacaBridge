# Task clock

Status: accepted

## Context

Every driver timer waits on the wall clock, and so do its tests. At f27c3e1, production code under `AlpacaCore/src/` and `AlpacaCore/include/` has 125 raw sleep lines across 34 files, and the tests have 149 lines that mention `sleep_for` (143 calls; the other 6 are comments), 57 of them in `AlpacaCore/tests/test_skywatcher_async.cpp`. A test that sleeps to wait for a driver timer is slow when the sleep is generous and flaky when it is not (#535). The SkyWatcher, Celestron and SynScan telescope drivers each carry a `task_wait_for` for their cancellable task waits, and SkyWatcher also sleeps directly in its poll, settle and autohome paths. Of the shared wait and deadline helpers, only `util::StreamLinkHealth` (`AlpacaCore/include/alpacacore/util/link_health.h`) and the client-silence watchdog's `note_client_activity` and `stop_motion_if_client_silent` (`AlpacaCore/include/alpacacore/telescope_driver.h`) take a `now` parameter today. `util::ConsecutiveSettle` (`AlpacaCore/include/alpacacore/util/poll_settle.h`) is clock-free by design, since its poll count is the elapsed time, and `PolledLinkHealth` counts failures.

## Decision

Every driver wait and deadline goes through an injected `TaskClock` with `now()`, a cancellable `wait_for` over the caller's mutex, condition variable and predicate, and a plain `sleep_for`. It is real by default and fake in tests. The real adapter wraps `std::chrono::steady_clock` and `std::condition_variable::wait_for`. The fake holds virtual time: `advance()` wakes every waiter whose deadline has passed, and `wait_for_waiters(n)` is the rendezvous that replaces sleep-and-hope in tests.

The fake's `advance()` takes each due waiter's mutex before notifying it, so a wakeup cannot fall between the waiter's predicate check and its block. The test thread that calls `advance()` holds no driver lock.

Once a driver is on the clock, every wait in it goes through the clock: the task waits, the rate-verify window, and the raw settle and poll sleeps. A test that sleeps to wait for a driver timer on such a driver is a review-blocking regression. `StreamLinkHealth` callers take `now()` from the clock; `ConsecutiveSettle` and `PolledLinkHealth` stay clock-free.

Pointing time and clock-step detection stay on the seams of [decision 0001](0001-skywatcher-pointing-clock.md). This clock is for waits and deadlines, never for LST or UTCDate.

## Alternatives rejected

Keep wall-clock waits and shorten the test timeouts: that trades a slow suite for a flaky one, which #535 already is. A per-driver clock parameter or template: the `task_wait_for` bodies would stay one copy per driver and each driver's tests would build their own fake. Put pointing time on the same clock: pointing time is wall time with a client-offset policy and a step detector, which is a different contract from a monotonic deadline; merging them would reopen decision 0001.

## Consequences

Drivers move onto the clock one at a time, starting with the SkyWatcher, Celestron and SynScan telescope drivers that share `task_wait_for`; a driver not yet moved keeps its raw sleeps, and the rule above applies only from the move on. The fake's wakeup rule needs a ThreadSanitizer case for the window between predicate check and block. The async operation slot and the state snapshot, which come later, test through this clock rather than through sleeps.

## Links

- Upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584); issues [#535](https://github.com/open-astro/AlpacaBridge/issues/535), [#547](https://github.com/open-astro/AlpacaBridge/issues/547), [#608](https://github.com/open-astro/AlpacaBridge/issues/608).
- Current waits this replaces: `task_wait_for` in `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`, `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` and `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp`.
- Related: [decision 0001](0001-skywatcher-pointing-clock.md) (pointing time); module overview in [architecture](../architecture.md#modules).
