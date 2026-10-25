# Camera capture session

Status: proposed

## Context

Five SDK cameras (gphoto, Player One, SVBONY, ToupTek, QHY) each ran a hand-rolled exposure thread with the same skeleton: a lifecycle mutex, an active flag, a deadline, a CameraState getter that forced Idle past the deadline, image flags, and an unbounded join on stop and disconnect. Four of them logged a failed exposure and left `ImageReady` false forever, so a client polling `ImageReady` never learned of it. QHY bounded the join and then detached the thread ([decision 0006](0006-async-operation-ownership.md), AGENTS.md: never detach a thread that touches `this`). The Player One copy was the pilot (issue #1605); the module is `util::CaptureSession` (`AlpacaCore/include/alpacacore/util/capture_session.h`).

## Decision

A camera's exposure runs in a `util::CaptureSession`. The session owns the state (Idle, Exposing, Download, Ready, Failed), the `LastExposure*` values, the kept failure, the watchdog deadline on the task clock ([decision 0005](0005-task-clock.md)) and one `AsyncOperation` slot ([decision 0006](0006-async-operation-ownership.md)) that runs the vendor body. The driver keeps the SDK sequence: it starts the exposure, waits through `CaptureControl::wait_for`, downloads, and publishes the frame through the control. Periodic workers (the QHY cooler-off, temperature and telemetry workers, the ToupTek thermal and pulse workers, the QHY CFW3 move) are not exposures and are not sessions.

A failed exposure keeps its message: `ImageReady` and `ImageArray` raise it as `DriverException`, `CameraState` reads Idle, and the next `StartExposure` or a reconnect clears it. `PercentCompleted` is valid only while Exposing; the driver base class applies that rule.

## Rules

1. A camera's exposure runs in a `util::CaptureSession`; the driver holds no exposure `std::thread`. Scope: `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: the Adoption table below (check 23) and review (a camera driver that spawns its own exposure thread).
2. One body runs per camera. `start()` refuses while an exposure is in progress; it waits at most 2 s for an old body, a cancel, or an abort's or a disconnect's join, then refuses with `InvalidOperation`. Two starts that wait together start one body, and a join never cancels a body that a later start began. Scope: `AlpacaCore/include/alpacacore/util/capture_session.h`. Checked by: `AlpacaCore/tests/test_capture_session.cpp` ("start refuses after kStartTimeout when the old body stays in the SDK", "two starts over a body still in the SDK start one body and getters never wait", "a start during an abort's join waits for it and its body is not cancelled").
3. A failed exposure keeps its message. `ImageReady` and `ImageArray` raise it as `DriverException`; `CameraState` is Idle; the next `StartExposure` or a reconnect clears it. Scope: `AlpacaCore/include/alpacacore/util/capture_session.h`, `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: `AlpacaCore/tests/test_capture_session.cpp` ("a body that throws fails the exposure until the next start"), `AlpacaCore/tests/test_playerone_camera.cpp` ("Reconnect clears a kept exposure failure and the last exposure").
4. Every wait of the body goes through `CaptureControl::wait_for` (the task clock) and is bounded by the watchdog deadline. Scope: `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: `AlpacaCore/tests/test_capture_session.cpp` ("the watchdog ends a body that waits on the clock"), `AlpacaCore/tests/test_playerone_camera.cpp` ("readiness poll gives up at the deadline on the task clock").
5. The driver stops the SDK exposure before it calls `abort()` or `disconnect_and_join()`, and a body that ends early stops the SDK exposure it started. `abort()` joins; its worst case is one readiness poll step or one `get_image_data` timeout. Scope: `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: `AlpacaCore/tests/test_playerone_camera.cpp` ("AbortExposure wakes the readiness poll without a clock advance", "a cancelled exposure body stops the SDK exposure").
6. The session mutex is a leaf: the third leaf under the driver `mutex_`, beside the `StateSnapshot` and slot mutexes, and it never nests with the slot mutex. Joins serialise on a join mutex taken without `mutex_`. `start()`, `abort()`, `disconnect_and_join()` and the destructor run without the driver `mutex_`. Scope: `AlpacaCore/include/alpacacore/util/capture_session.h`, `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: `AlpacaCore/tests/test_capture_session.cpp` ("lock-order storm: getters under the driver mutex race every session call"), review (a session call made under `mutex_`).
7. A camera that reports the actual exposure length adds it in its own slice. Scope: `AlpacaCore/include/alpacacore/util/capture_session.h`. Checked by: review (the slice that adds the parameter).
8. `disconnect_and_join()` closes the session. `start()` on a closed session raises `NotConnected` without publishing a state or running a body, a start that is waiting at that moment included. The driver calls `open()` on connect, and again when a disconnect leaves the camera connected; `open()` refuses while a disconnect is still running. Scope: `AlpacaCore/include/alpacacore/util/capture_session.h`, `AlpacaCore/src/vendors/*/*camera_driver.cpp`. Checked by: `AlpacaCore/tests/test_capture_session.cpp` ("disconnect_and_join closes the session: start raises NotConnected until open", "a start waiting when disconnect_and_join begins is refused and never runs after the clear"), `AlpacaCore/tests/test_playerone_camera.cpp` ("a disconnect that leaves the camera connected leaves the session open").

## Adoption

The roster of camera drivers whose exposure runs in a session. `scripts/check_docs_drift.py` check 23 holds it to the code.

Scan: `AlpacaCore/src/vendors/*/*camera_driver.cpp`
Symbol: `CaptureSession`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Player One camera | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | adopted | #1605 | pilot; no `detach()` left |
| ZWO camera | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | planned | | gains a body; today it polls the SDK in its getters |
| ToupTek camera | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | planned | | Altair rides this driver |
| SVBONY camera | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | planned | | |
| gphoto camera | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | planned | | adds the actual exposure length (rule 7) |
| QHY camera | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | planned | | last: `GetQHYCCDSingleFrame` has no timeout, and its `detach()` sites go with the slice |

## Alternatives rejected

- **Six implementations.** Four could not tell a client that an exposure failed, and the copies had drifted apart.
- **A `CameraDriverBase` that owns the exposure thread.** A base class is the shape [decision 0013](0013-designs-to-avoid.md) rejects; the driver composes the session.
- **A session that owns the SDK calls through one generic camera interface.** The six SDKs differ (status polling, a blocking frame call, a video stream, an event pump); one interface would be the universal vendor adapter. The body stays in the driver.
- **Keep QHY's bounded join and then detach.** AGENTS.md forbids detaching a thread that touches `this`; decision 0006 bounds the problem with a stale list and a refusal.
- **A sticky `cameraError` state after a failure.** ConformU 4.4.0 gave up 17 tests on a camera that stayed in Error; the failure is raised by `ImageReady` and `ImageArray` instead.

## Consequences

- The driver deletes its exposure thread, lifecycle mutex, active flag, deadline and joins; Player One's `git diff --numstat` for the slice is in its PR.
- A StartExposure that races a disconnect raises `NotConnected` from the session, not `InvalidOperation`. One case stays open by design: a start that passed its checks, then sleeps across a whole disconnect and a new connect, runs its body on the new connection with settings it read before.
- The failure surface changes for the four cameras that logged only: `ImageReady` and `ImageArray` raise `DriverException` after a failed exposure. Each slice names this in its PR.

## How to reverse

Each camera adopts in its own PR; reverting that PR restores the camera's thread code. To drop the rule, supersede this record, mark the Adoption rows `planned` or remove them, and restore the camera exposure-worker text in the driver lifecycle instructions. The module is additive and stays unused by a camera that has not adopted it.

## Links

- Issues [#1605](https://github.com/open-astro/AlpacaBridge/issues/1605) (Player One pilot).
- [Decision 0005](0005-task-clock.md), [decision 0006](0006-async-operation-ownership.md), [decision 0013](0013-designs-to-avoid.md), [decision 0016](0016-getters-never-wait-on-device-io.md).
- Rules: [driver lifecycle instructions](../../.github/instructions/driver-lifecycle.instructions.md), [ASCOM contract instructions](../../.github/instructions/ascom-contract.instructions.md).
