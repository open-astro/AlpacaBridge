# Task clock

Status: accepted

## Context

Every driver timer waits on the wall clock, and so do its tests. At f27c3e1, production code under `AlpacaCore/src/` and `AlpacaCore/include/` has 125 raw sleep lines across 34 files, and the tests have 149 lines that mention `sleep_for` (143 calls; the other 6 are comments), 57 of them in `AlpacaCore/tests/test_skywatcher_async.cpp`. A test that sleeps to wait for a driver timer is slow when the sleep is generous and flaky when it is not (#535). The SkyWatcher, Celestron and SynScan telescope drivers each carry a `task_wait_for` for their cancellable task waits, and SkyWatcher also sleeps directly in its poll, settle and autohome paths. Of the shared wait and deadline helpers, only `util::StreamLinkHealth` (`AlpacaCore/include/alpacacore/util/link_health.h`) and the client-silence watchdog's `note_client_activity` and `stop_motion_if_client_silent` (`AlpacaCore/include/alpacacore/telescope_driver.h`) take a `now` parameter today. `util::ConsecutiveSettle` (`AlpacaCore/include/alpacacore/util/poll_settle.h`) is clock-free by design, since its poll count is the elapsed time, and `PolledLinkHealth` counts failures.

## Decision

Every driver wait and deadline goes through an injected `TaskClock` with `now()`, a cancellable `wait_for` over the caller's mutex, condition variable and predicate, and a plain `sleep_for`. It is real by default and fake in tests. The real adapter wraps `std::chrono::steady_clock` and `std::condition_variable::wait_for`. The fake holds virtual time: `advance()` wakes every waiter whose deadline has passed, and `wait_for_waiters(n)` is the rendezvous that replaces sleep-and-hope in tests.

The fake's `advance()` takes each due waiter's mutex before notifying it, so a wakeup cannot fall between the waiter's predicate check and its block. The test thread that calls `advance()` holds no driver lock.

Once a driver is on the clock, every wait in it goes through the clock: the task waits, the rate-verify window, and the raw settle and poll sleeps. A test that sleeps to wait for a driver timer on such a driver is a review-blocking regression. `StreamLinkHealth` callers take `now()` from the clock; `ConsecutiveSettle` and `PolledLinkHealth` stay clock-free.

Pointing time and clock-step detection stay on the seams of [decision 0001](0001-skywatcher-pointing-clock.md). This clock is for waits and deadlines, never for the LST or UTCDate value; [decision 0014](0014-utc-offset.md) splits the two: the UTC value stays on `system_clock`, and only the host time elapsed since the write reads this clock.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. In a driver whose Adoption row is adopted, every wait, sleep and deadline reads the injected `TaskClock`. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review; the roster by `scripts/check_docs_drift.py` check 23.
2. A test of such a driver waits for a driver timer through the fake clock (`advance()`, `wait_for_waiters()`), never with `sleep_for`. Scope: `AlpacaCore/tests/test_*.cpp`. Checked by: review.
3. The task clock never supplies a UTC or LST value; only the elapsed time since a UTCDate write reads it ([decision 0014](0014-utc-offset.md)). Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review.
4. `StreamLinkHealth` callers pass `now()` from the clock; `ConsecutiveSettle` and `PolledLinkHealth` take no clock. Scope: `AlpacaCore/include/alpacacore/util/*.h`, `AlpacaCore/src/vendors/**`. Checked by: review.
5. The fake's `advance()` takes each due waiter's mutex before it notifies it. Scope: `AlpacaCore/tests/fake_task_clock.h`. Checked by: `AlpacaCore/tests/test_task_clock.cpp`.

## Alternatives rejected

Keep wall-clock waits and shorten the test timeouts: that trades a slow suite for a flaky one, which #535 already is. A per-driver clock parameter or template: the `task_wait_for` bodies would stay one copy per driver and each driver's tests would build their own fake. Put pointing time on the same clock: pointing time is wall time with a client-offset policy and a step detector, which is a different contract from a monotonic deadline; merging them would reopen decision 0001.

## Consequences

The Adoption table below is the current roster; this paragraph is the history of the moves. Drivers move onto the clock one at a time, starting with the SkyWatcher, Celestron and SynScan telescope drivers that share `task_wait_for`; the iOptron telescope driver followed (#1223): its slew poll and settle sleeps, deadlines and override windows read the clock, and its clock-sync body waits through it; the ToupTek camera and filter wheel drivers followed (#1268): the camera's pulse-guide and thermal-poll waits, exposure deadline and thermal cache ages, and the wheel's homing poll sleep read the clock; the Player One camera driver followed (#1279): its pulse-guide timer, exposure deadline and image-readiness poll read the clock; the QHY camera driver followed (#1386): its exposure watchdog, pulse-guide timer, settle sleep, worker waits and 2 s reap loops read the clock, and the QHY filter wheel's own settle-clock function became the same `TaskClock`; the Bisque telescope (#1162), the SVBONY and gphoto cameras (#1282) and the ZWO camera (#1328) followed; the Astroasis focuser, the Gemini focuser and flat panel, the QHY Q-Focuser and the Celestron focus motor (#1320) were on the clock from their first slot; the WandererAstro cover, filter wheel and box read it for link-health timing (#730); the OnStep telescope moved with its first slot, the MoveAxis re-send (#1378): its status and position cache ages, slew grace, pulse-guide end, synchronous-slew poll and settle sleeps, re-send due times and the elapsed time since a UTCDate write read the clock; the Celestron CGX telescope takes the clock but still has a wall-time deadline, and moves in #1404; a driver not yet moved keeps its raw sleeps, and the rule above applies only from the move on. The fake's wakeup rule needs a ThreadSanitizer case for the window between predicate check and block. The async operation slot and the state snapshot, which come later, test through this clock rather than through sleeps.

## Adoption

The roster of drivers on the clock. It covers driver files; a protocol wrapper's I/O timeouts are not driver waits. `scripts/check_docs_drift.py` check 23 fails when a driver names `TaskClock` without a row, or a row disagrees with its source.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `TaskClock`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Sky-Watcher telescope | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #751, #917 | UTC anchors stay off the clock ([decision 0001](0001-skywatcher-pointing-clock.md)); position model clock in #1405 |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #923 | |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | partial | #1403 | tick-hunt wait still on `steady_clock` |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | adopted | #1223 | |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1162 | |
| Celestron CGX telescope | `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp` | partial | #1404 | continuity-check deadline on wall time |
| ToupTek camera | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | adopted | #1268 | |
| ToupTek filter wheel | `AlpacaCore/src/vendors/touptek/touptek_filterwheel_driver.cpp` | adopted | #1268 | |
| Player One camera | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | adopted | #1279 | |
| QHY camera | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | adopted | #1386 | |
| QHY filter wheel | `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp` | adopted | #1386 | |
| QHY Q-Focuser | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted | #1310 | on the clock from its first slot |
| SVBONY camera | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | adopted | #1282 | |
| gphoto camera | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | adopted | #1282 | |
| ZWO camera | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | adopted | #1328 | |
| Astroasis focuser | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted | #1327 | on the clock from its first slot |
| Gemini focuser | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | adopted | #1229 | on the clock from its first slot |
| Gemini flat panel | `AlpacaCore/src/vendors/gemini/gemini_flatpanel_driver.cpp` | adopted | #1255 | |
| Celestron focus motor | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted | #1320 | on the clock from its first slot |
| iOptron iGuider camera | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | adopted | | on the clock from its first slot; exposure, stall and first-frame deadlines |
| WandererAstro switch box | `AlpacaCore/src/vendors/wandererastro/wandererastro_box_switch_driver.cpp` | adopted | #730 | link-health timing |
| WandererAstro cover | `AlpacaCore/src/vendors/wandererastro/wandererastro_covercalibrator_driver.cpp` | adopted | #730 | link-health timing |
| WandererAstro filter wheel | `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_driver.cpp` | adopted | #730 | link-health timing |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1378 | moved with its first slot (the MoveAxis re-send) |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | planned | — | |
| iOptron iEAF focuser | `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_focuser_driver.cpp` | planned | — | |

## Links

- Upstream design review [#584](https://github.com/open-astro/AlpacaBridge/issues/584); issues [#535](https://github.com/open-astro/AlpacaBridge/issues/535), [#547](https://github.com/open-astro/AlpacaBridge/issues/547), [#608](https://github.com/open-astro/AlpacaBridge/issues/608).
- Current waits this replaces: `task_wait_for` in `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`, `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` and `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp`.
- Related: [decision 0001](0001-skywatcher-pointing-clock.md) (pointing time); module overview in [architecture](../architecture.md#modules).
