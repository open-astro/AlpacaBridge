---
applyTo: "AlpacaCore/tests/**,AlpacaHTTP/tests/**,AlpacaCore/conformu/**"
---

## Tests, fakes and ConformU

Shared rules for every vendor, moved from `AGENTS.md` (which keeps the core rules
and routes here) and, for the QHY `before_call` exception, from the ci-gates rules.
A rule that applies to a second vendor belongs in the matching shared file, not in a
vendor file.

## Testing Requirements

Related failure: [Release builds disabled HTTP assertions](../../docs/failures/0004-ndebug-disabled-http-assertions.md).

- Non-trivial code must have unit tests under `AlpacaCore/tests/` or `AlpacaHTTP/tests/`.
- Build driver targets and test targets together.
- Tests should be runnable via `run_all_tests.sh`.
- Preferred test naming:
  - `test_<component>.cpp`
  - `test_<vendor>_<device>.cpp`
- Use tags to separate unit/integration/hardware behavior when applicable.
- Use Catch2 macros (`REQUIRE`, `CHECK`, `CHECK_THROWS_AS`, etc.) via the `catch2_compat.h` header.
- **AlpacaHTTP hand-rolled tests must not use `assert()`.** The AlpacaHTTP tests (`test_routing`, `test_json`, `test_config`, `test_discovery`) are plain `int main()` programs, not Catch2. They use the always-on `EXPECT()` macro from `AlpacaHTTP/tests/test_assert.h`. Never use `<cassert>` `assert()` there: it is compiled out under `-DNDEBUG` — which Release, `debian/rules`, and the shipped `.deb` all define — so an assert-based check silently does nothing in an optimized build. Worse, an `assert(side_effecting_call())` (e.g. `assert(request.parse(...))`) means the call itself never runs under `NDEBUG`, so the test exercises nothing and can crash on the resulting empty state. `run_all_tests.sh` and CI build *without* `NDEBUG`, so this class of bug hides until someone builds Release. `EXPECT()` evaluates its expression exactly once and aborts on failure regardless of build type.

### New test cases name their falsifying mutation

A test case that has never failed pins nothing. Every case a PR adds or renames is listed in the PR body in a `## Falsified by` section after `## What Changed`, one line each: `- "<case name>": <path>:<line> <change>`, where `<path>:<line>` is production code (under `AlpacaCore/src`, `AlpacaCore/include`, `AlpacaHTTP/src` or `AlpacaHTTP/web`) and `<change>` (at least three words) is the one edit that makes the case fail. A test-helper path is accepted only when the case is about the helper (`Fake*`, `PtyPair`, `StressCallGuard`, `[stress-guard]`, `[driver_rig]`). New means a Catch2 registration macro, a `test('...')` in `AlpacaHTTP/tests/web/`, or a top-level `main()` block in an AlpacaHTTP hand-rolled test that opens with `// case: <name>`, whose name is absent from that file at the merge-base, unless the base declared it in a file that no longer does (a moved case); a renamed case is new. A hand-rolled file whose `main()` is flat (no scoped `    {` blocks) never produces a new case. A merge-down PR (head `merge-down/X.Y-to-<main|X.Z>` against that base, adding no commit that is not already on `stable/X.Y`; `scripts/merge_down.py`, `docs/beta-channel.md`) is exempt: its cases were listed on the fix PRs that landed them on `stable/X.Y`. `scripts/check_falsified_by.py` checks the shape only (`--self-test` covers the parser); the review chain applies the mutation and confirms the case fails.

### Required Test Cases for Every New Vendor Device Driver

Every new driver **must** ship with at least the following 8 unit test cases, plus the config round-trip test (case 9). Use the existing tests (e.g. `test_svbony_camera.cpp`, `test_gemini_focuser.cpp`) as reference.

Cases 3 and 4 are satisfied by the driver's registry entry in `contract_sweep.h`: the tier 1 contract sweep (`test_contract_sweep.cpp`) runs the NotConnected codes, the unsupported `Action` and `Command*` codes, and the error vocabulary of those calls for every entry. It covers case 6 only in part: InvalidValue before NotConnected on its static out-of-range probes, while disconnected, and an entry with no such probe registers no such case and states why in `no_invalid_probe_reason`; a guard case fails when a tier 1 case is registered with an empty probe list. It does not cover case 8: no sweep case calls a device-specific member the driver does not support. The per-file minimum is therefore cases 1 (Defaults), 2 (Device metadata, the strings only the vendor file knows), 5 (device-specific), 6 (the range checks the sweep's probes do not reach), 7 (state machine), 8 (unsupported members) and 9 (config round-trip). A vendor file may still carry 3 or 4 for a driver whose entry has an exception, and says why in a comment. Cases 3, 4 and 6 below describe what the sweep, or that file, must assert.

Shared test helpers live in `AlpacaCore/tests/driver_rig.h` (namespace `alpacacore::test`): `require_alpaca_error(fn, code)`, `timed(fn)`, `parse_timestamp(text)` and `time_stamp_of(state)`. A test file includes it and never defines its own copy, under any name, and a helper two files need goes there. The rule covers copied helpers: a timestamp parse written inline in a test body (for example in `test_celestron_focuser.cpp`, `test_zwo_telescope.cpp`) is not one, and moves to `parse_timestamp` when its test changes. A helper that adds a check of its own (message text, a label, a choice of codes) or swallows an error (`elapsed_of` in `test_touptek_focuser.cpp` and `time_of` in `test_touptek_fake_sdk.cpp`, each a `timed` that drops the `AlpacaException`) may stay in its file; `camera_percent_cooler_checks.h` keeps its `require_error_code` until the per-file cases the sweep covers move to it. A test of a driver whose Adoption row in [decision 0005](../../docs/decisions/0005-task-clock.md) is `adopted` waits through the fake clock (`wait_for_waiters`, `advance`), never `std::this_thread::sleep_for`; a sleep that stays carries a comment naming the real-time thing it waits for. The rule is rule 2 of decision 0005, which owns it. Tests written before their driver moved onto the clock still sleep (for example `test_skywatcher_motion_limits.cpp`, `test_qhy_camera.cpp`, `test_bisque_telescope.cpp`; gphoto in #1587); a change to the driver-timer waits of such a test moves them onto the fake clock.

1. **Defaults** `"<Vendor> <Device> Driver - Defaults"` `[<vendor>][<device>][unit]`
   - Create driver with device number 0.
   - `REQUIRE` device type, device number, and `get_connected() == false`.
   - `CHECK` the default device name.
   - `CHECK` any static capability flags (e.g. `get_can_abort_exposure`, `get_can_reverse`, `get_absolute`).

2. **Device metadata** `"<Vendor> <Device> Driver - Device metadata"` `[<vendor>][<device>][unit]`
   - Create driver with a non-zero device number (e.g. 3) so `get_unique_id()` is distinguishable.
   - `CHECK` all of: `get_device_number`, `get_description`, `get_driver_info`, `get_driver_version`, `get_interface_version`, `get_unique_id`.
   - String values must match the implementation exactly — read the driver source to get the correct strings.

3. **Not connected throws / Disconnected behavior** `[<vendor>][<device>][unit]`
   - Verify that operations requiring a live connection throw `alpacacore::AlpacaException` (or return safe defaults where the driver explicitly does so — document why in a comment).
   - Cover the device's primary operations (e.g. for cameras: `get_gain`, `start_exposure`, `get_image_array`; for telescopes: `get_right_ascension`, `get_tracking`, `slew_to_target_async`).

4. **Unsupported actions** `[<vendor>][<device>][unit]`
   - `CHECK` `get_supported_actions()` is empty (unless the driver defines actions).
   - `CHECK` `can_action("anything") == false`.
   - `require_alpaca_error` with `ActionNotImplemented` (0x40C) for `action()`, and with `MethodNotImplemented` (0x400) for `command_blind()`, `command_bool()`, `command_string()` (unless the driver implements command pass-through: a pass-through driver checks the connection first, so disconnected it throws `NotConnected` (0x407), and the test asserts that instead) — the codes case 8 and the [exception table](ascom-contract.instructions.md#ascom-exception-vocabulary-pick-the-right-one--conformu-checks-it) require. A bare `CHECK_THROWS_AS(..., AlpacaException)` passes for any error code, so it cannot catch the wrong one.

5. **Device-specific behavior** — at least one test covering behavior unique to the device type:
   - Cameras: sub-exposure support (`get_sub_exposure_duration` / `set_sub_exposure_duration` throw if unsupported).
   - Telescopes: target coordinate validation, axis rate ranges.
   - Focusers: `get_absolute`, `get_temp_comp_available`, device state telemetry.
   - Filter wheels: names/offsets defaults, invalid position handling.
   - Switches: `get_max_switch`, invalid switch ID handling.
   - Rotators: `get_can_reverse`, device state telemetry.

6. **Value range validation** — invalid inputs must throw `AlpacaException` with `error_code() == AlpacaError::InvalidValue`, not silently normalize or throw a generic error (ConformU specifically tests boundary values).

7. **State machine contracts** — device state follows ASCOM rules without needing hardware (e.g. `CameraState == Idle` before any exposure, `Slewing == false` when not connected, `IsPulseGuiding == false` when idle). These have caught real bugs: iOptron's settle loop prematurely declared slews complete, SynScan's `IsPulseGuiding` always returned false, SVBONY's `CameraState` got stuck after SDK hangs.

8. **Unsupported method error codes** — a method the device doesn't support must throw `MethodNotImplemented` (or `PropertyNotImplemented` for a property, both 0x400), not a generic `DriverException`. `InvalidOperation` (0x40B) is for a *supported* member called in a state where it can't currently run, not for a member the hardware lacks — see the [ASCOM exception vocabulary table](ascom-contract.instructions.md#ascom-exception-vocabulary-pick-the-right-one--conformu-checks-it). ConformU distinguishes "not implemented" from "driver error." `Action()` called with a name not in `SupportedActions` throws `ActionNotImplemented` (0x40C) instead — a distinct code, not `MethodNotImplemented`.

   Cases 6-8 are the **ASCOM contract tests**: they exist specifically because a generic "does it throw?" test (case 3/4) is not enough to pass ConformU, which checks the exact Alpaca error code and state-machine behavior. See `/driver-build` Step 7 for the full pattern and worked examples per device type, and `require_alpaca_error` in `AlpacaCore/tests/driver_rig.h`. **The vendor file and its sweep entry together cover all 8 cases, 30+ assertions total** — cases 6-8 alone should add 10-15 assertions on top of the 5 basic cases; if you have significantly fewer you are probably not testing enough error codes and state transitions.

9. **Config save→load round-trip** in `AlpacaHTTP/tests/test_routing.cpp` — `configuredevice` then read back `configureddevices` and assert **every persisted field survives** (index/id, filter names, PWM/port config, etc.). The automated catch for the two silent-data-loss classes described in [Enumeration index fields](http-integration.instructions.md#enumeration-index-fields--unique-names--auto-numbering-all-vendors). Model it on the existing ToupTek AFW filter-wheel round-trip test. This is an `AlpacaHTTP`-level integration test, additional to the 8 vendor unit tests above, not a substitute for cases 6-8.

### Hardware-free driver tests via the SDK seam (ToupTek, QHY, gphoto and SVBONY camera — extend to other vendors)

Related decision: [SDK seams](../../docs/decisions/0015-sdk-seams.md) (why one seam per vendor library, the destructor shape and the Locked decorator).

The SVBONY camera has the same seam (`SVBSDK` in `svbony_sdk.h`, `FakeSVBSDK`, `LockedSVBSDK`, issue #1010).
The Player One camera and Phoenix Wheel have it too: `PlayerOneSDK` / `FakePlayerOneSDK` / `LockedPlayerOneSDK` for the camera, and `PlayerOnePWSDK` (`playerone_pw_sdk.h`) / `FakePlayerOnePWSDK` / `LockedPlayerOnePWSDK` for the wheel, whose `create_playerone_filterwheel(device, index, sdk)` overload leaves the two-argument factory on the real singleton (issue #1256).

The ToupTek drivers take the SDK through the abstract `ToupTekSDK` interface
(`touptek_sdk_wrapper.h`): production factories pass the `ToupTekSDKWrapper`
singleton; every factory has an overload taking a `ToupTekSDK&` that tests use
to inject the scripted `FakeToupTekSDK` (`tests/fake_touptek_sdk.h` — throws
from any named call, canned enumerations, ref-counted open/close counting,
scripted wheel-position sequences). This makes the highest-risk paths —
error/throw cleanup, ref-count balance, reconnect, enumeration index math —
unit-testable without hardware (`test_touptek_fake_sdk.cpp`). Rules:

- New ToupTek driver code must reach the SDK only through the injected `sdk_`
  member, never `ToupTekSDKWrapper::instance()` directly.
- A connect-path or cleanup fix in a ToupTek driver should come with a fake-SDK
  test reproducing the failure (throw from the exact call that regressed).
- When touching another vendor's wrapper significantly, adopt the same seam
  shape there (one abstract interface + factory overload + scripted fake) —
  the reusable pattern from issue #104. The gphoto camera has the same seam since
  #546 (`GPhotoSDK` / `FakeGPhotoSDK` / `LockedGPhotoSDK`, plus a `RawDecoder`
  seam for libraw). The two original seams differ in one
  detail worth copying deliberately rather than by accident: `ToupTekSDK` has
  a public virtual destructor, `QHYSDK` a protected non-virtual one. **Prefer
  the QHY form for a new seam.** Nothing owns a seam pointer in either design
  (drivers hold a reference, workers capture a raw pointer, implementations
  are statics or stack objects), and the protected non-virtual destructor is
  what stops `delete` through the interface from compiling at all. ToupTek's
  public virtual destructor predates that reasoning; it is harmless there and
  not worth churning, but it is not the shape to copy.
- **QHY has the same seam** (`QHYSDK` / `FakeQHYSDK` / `LockedQHYSDK`, issue
  #321), and for QHY it is the *only* way to test a connect at all: the first
  `libqhyccd` call spawns `PnpEventListenerThread`, which segfaults in
  `libusb_hotplug_register_callback` on any host without a working USB stack,
  so the real singleton cannot be touched on a test runner. Two extra rules
  apply there. **(a) No QHY fake method may block** — the camera driver's
  exposure, temperature, cooler-off and telemetry workers join with a bounded
  timeout and *detach* on expiry (telemetry since #323), and its pulse-guide
  worker is detached by design, so a
  blocking fake leaves detached threads calling into it after the test ends.
  **(b) Those detachable workers must reach the seam through a captured
  `QHYSDK*`, never through `this->sdk_`** — the workers still capture `this`
  for everything else they touch; only the SDK call is required to go through
  the raw pointer. **This narrows the use-after-free window; it does not close
  it.** A detached worker that outlives the driver still dereferences `this`
  afterwards (`connected_`, `mutex_`, the `exposure_superseded` re-check), and
  that is UB whichever form the SDK call takes. What the raw pointer buys is
  the *long* part of the window: the seam object is guaranteed to outlive
  every driver built on it (rule 2 in `fake_qhy_sdk.h`), so a worker parked
  inside a blocking SDK call — where it spends essentially all its time — is
  not holding a reference into the driver for that whole duration. Reaching
  the SDK via `sdk_` would instead touch the driver at the moment each call
  returns, on top of the post-call touches. Treat detached workers as unsafe
  and bound their lifetime; do not read this rule as making them safe.
  Rule (a) has one sanctioned escape hatch: `FakeQHYSDK::before_call`, a hook that
  is null in every ordinary test and that a case sets deliberately to make one
  named fake method block (the three #339 cases use it to prove the
  slowest-forward watchdog, the cancel forward's own timing and the cancel
  overtake); a case that sets it owns the consequences and must release the
  block before its driver is destroyed.
- **When a fake and the real SDK disagree, the fake must be the HARSHER of the
  two** (`fake_qhy_sdk.h`, issues #373/#365/#390). A fake that answers a
  plausible value where hardware answers a sentinel, or that settles instantly
  where hardware converges, produces green tests for driver code that breaks on
  the bench — and the plausible answer is the dangerous one precisely because
  nothing looks wrong. Three QHY examples, two now fixed and one re-scoped and all worth
  recognising in the next fake: `get_param()` answered `0.0` for an unsupported
  control where `GetQHYCCDParam()` answers `QHYCCD_ERROR` (~4.29e9), so
  "unsupported" and "reads zero" were indistinguishable; `get_mem_length()`
  was reported as ignoring the binning it had been told about, and acting on
  that entry literally made the fake worse, which is its own lesson: **a gap
  entry is a claim about the real SDK, and it can be wrong.** Check the
  units against the only caller before "fixing" one. It also exposed a
  second, sharper rule: **a fake's paired calls must agree with each other**,
  since `get_mem_length()` and `get_single_frame()` are used together (size a
  buffer from one, fill it with the other) and hardware cannot deliver an
  image larger than `GetQHYCCDMemLength()`. Fixing one of a pair alone turned
  a parity gap into a heap-buffer-overflow inside the fake, which reads as a
  driver bug in an ASan/TSan job. Write the units down where the state lives
  (`roi_` is in binned pixels, because that is what the driver passes) and
  pin the pairing with a case, not just the single call; and
  `control_temp()` wrote its target straight into `CURTEMP`, an instant settle
  the real `ControlQHYCCDTemp` PID can never produce, which would have let a
  driver that merely reads back its own setpoint pass a thermal test. Keep the
  `KNOWN PARITY GAPS` block at the top of a fake exhaustive, and prefer closing
  a gap to documenting it. **Give a fake's shared setup ONE home**: the
  one-camera `make_fake()` was copied verbatim into three QHY test files
  (issue #342) and is now `FakeQHYSDK::with_one_camera()` next to the canned
  camera it builds, so a change to what a default test fake looks like cannot
  be made in two files out of three.
- **Poll-until-settled loops keep the sleep cadence in the driver but put the
  DECISION in `util::ConsecutiveSettle`** (`util/poll_settle.h`, issue #105):
  stability-run + poll-budget semantics, unit-tested with scripted sequences
  (`test_poll_settle.cpp` — bounce, fast-homer, timeout, final-poll settle).
  Use it for any new loop of that shape (the AFW `wait_for_home` is the
  reference); single-edge loops ("poll until IsSlewing flips") don't need it.
  One hand-rolled instance remains BY DESIGN: the iOptron slew-settle loop
  (`ioptron_telescope_driver.cpp`, `kRequiredStableReads`) shares the
  consecutive-run core but has a clock-deadline budget and a dual exit
  (target-reached OR stabilized) — forcing it onto the poll-budget API would
  fake a parameter. Unify if a third instance of that extended shape appears.

### pty-backed fakes: never write to the master with a blocking write

A fake that answers a driver over a pseudo-terminal must open its master
non-blocking and write through `pty_write_bounded()` from
`AlpacaCore/tests/fake_pty_write.h`, passing its own `stop_` flag. Open the
pair through `PtyPair` in that same header rather than a hand-rolled
`posix_openpt()` block: the hand-rolled shape leaked the master on every
setup-failure path and ignored a failed keep-alive open, and it had been
copied six times before #387 replaced every copy; all six fakes now hold a
`PtyPair`, and `test_fake_pty_write.cpp` pins its ownership contract. A bare
`write(master_fd_, ...)` on a blocking master parks the fake's worker thread as
soon as the driver stops draining — which is normal as a concurrency test winds
down — and the destructor's `join()` then never returns, because the thread is
asleep in `write()` and never reaches the `stop_` check. The result is a hung
process, not a failing test, and it is a race, so it shows up as an occasional
CI hang rather than a reproducible red (#424, the shape #364 describes).

Dropping a reply is the correct answer here: a reply the driver is not draining
is one it was never going to read, and a fake whose destructor can hang is worse
than one that drops a frame.

### Filter wheel move model: one case set over ZWO EFW and QHY CFW3 (#1629)

`AlpacaCore/tests/test_filterwheel_move_model.cpp` runs six case bodies unchanged over the ZWO EFW driver and the QHY CFW3 driver: `Position` is -1 while moving then the target, a valid write returns without waiting, an out-of-range write is `InvalidValue`, `InvalidValue` precedes `NotConnected`, a cache primed at connect needs no device read, and transit slots (including the pre-move slot) never reach `Position` (failure 0001). The bodies are templates over a rig struct; a rig only builds the driver, scripts a slow move and counts device reads. The QHY CFW3 passes all six. The ZWO EFW diverges on the last two: `set_connected(true)` only resets the TTL frame, so the first `Position` is an SDK read, and the driver has no pending target, so it passes only the SDK's own -1 through. Those two ZWO cases carry `[!shouldfail]`, so CI fails when the driver starts to pass them; remove the tag in the PR that fixes the driver.

- No shared filter wheel move-model module is extracted while a case diverges on a driver it would serve. The ZWO driver gets a pending target and a cache seeded at connect first.
- A wheel driver that joins the test adds a rig and the `TEST_CASE` pair; it does not edit or copy a case body.
- The ZWO upper slot bound (`set_position`) is not proven by the out-of-range case: the SDK fake also refuses the slot. The QHY transit rig only calls `slow_moves()`, so its transit case is the same evidence as the moving case.

### Test CMake Integration

When adding a test file for a new vendor device:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list in `AlpacaCore/tests/CMakeLists.txt`, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.
- Build and run all tests (`cmake -B build-vendors -DALPACACORE_ENABLE_<VENDOR>=ON && cmake --build build-vendors --target alpacacore_tests && ./build-vendors/tests/alpacacore_tests`, from `AlpacaCore/`) before considering the driver complete. Use `build-vendors`, not `build`: CMake caches `CMAKE_CXX_FLAGS` and the vendor options from the first configure of a directory, and a `build` under AlpacaCore/ may hold an earlier one (from #588 until #1251 a default pre-flight left a sanitized, vendors-OFF configure there; a full pre-flight, `PREFLIGHT_FULL=1`, now builds in the `build` directory under AlpacaHTTP/ and deletes the AlpacaCore one, and the default quick run builds nothing), so reusing it can compile no driver and stay sanitized whatever you pass.

### ConformU version (arm64 timing bug)

**ConformU 4.5.0 has a known arm64 timing bug — use 4.5.1+; the standard is 4.5.3 (build 56334, a GitHub release).** On arm64, the *first* Camera-device member returning each distinct .NET response type (`CameraState`/enum, `CameraXSize`/int, `SensorType`/enum) in a fresh ConformU process is charged ~0.13-0.22 s "OUTSIDE FAST RESPONSE TIME TARGET", while AlpacaBridge answers in <1 ms (confirmed via TRACE-level dispatch timing AND a loopback packet capture during SC715C validation, 2026-09-11: the request/response round trip was 3 ms; the ~140 ms was entirely client-side, between ConformU receiving the response and issuing its next request). Root cause: the official `conformu.linux-arm64.tar.xz` 4.5.0 release was accidentally published without `PublishReadyToRun`, so .NET JIT-compiles each new generic-over-value-type instantiation (`TimeFunc<CameraState>`, `TimeFunc<int>`, `TimeFunc<SensorType>`) on first use — reproducible on ANY vendor's camera driver on this rig (confirmed identically on ZWO ASI120MM Mini), not a driver bug. Filed and fixed upstream: [ConformU#31](https://github.com/ASCOMInitiative/ConformU/issues/31); the maintainer's 4.5.1 beta (rebuilt with `PublishReadyToRun`) was clean, and so are 4.5.2-rc.1 and 4.5.2; 4.5.3 is the same ReadyToRun (`PublishReadyToRun`) publish build with only the ConformU#36 fix added, and was pinned provisionally until a camera and a telescope run with it were recorded in `SUPPORTED-DRIVERS.md` (both recorded 2026-10-25: the ToupTek ATR2600M and GPCMOS01200KPF, and the TeSeek HM-17). 4.5.1 can no longer be downloaded (the beta URL now serves rc.1), and reports made with it stay valid, as do reports made with 4.5.2 (build 56139). Install the standard from the sha256-checked v4.5.3 release asset, as `/conformu` step 2g does — do NOT validate a driver's timing against a 4.5.0 arm64 run; a "3 members took longer than their target response times" result showing exactly these three members (and nothing else) is this bug, not a regression to chase in driver code. The same class of bug hit Telescope devices too (`AlignmentMode`, `EquatorialSystem`, `SideOfPier` — first use of each enum type).
