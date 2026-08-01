# Agent Instructions

This file is the single source of truth for agent behavior in this repository.

## Repository Structure and Build Output

- Keep build/output folders inside the owning project directory:
  - `AlpacaCore/build*`
  - `AlpacaHTTP/build*`
- Never create root-level ad-hoc build directories (examples to avoid: `build-synscan`, `build-temp`, `cmake-build-*` at repo root) unless explicitly requested.
- Keep generated artifacts out of source trees and avoid tracked build-system output files (Makefiles, CMake cache files, etc.) outside approved build folders.

## Core Architecture

- `AlpacaCore` is vendor-neutral Alpaca logic and device behavior only:
  - No HTTP/REST/sockets/JSON transport code.
  - Vendor SDKs isolated to `external/` + `src/vendors/<vendor>/` + `include/alpacacore/vendor/<vendor>/`.
- `AlpacaHTTP` is transport/routing/config/discovery only:
  - No vendor SDK use.
  - No duplicated device logic from `AlpacaCore`.
  - Web UI assets live in `AlpacaHTTP/web/` (HTML, CSS, JS). These are served by the HTTP server and packaged into the `.deb` at `/usr/share/alpacabridge/web/`.
- Call flow is always:
  - `AlpacaHTTP -> AlpacaCore driver -> vendor implementation`.

Supported device types (base drivers in `AlpacaCore/src/drivers/`): Camera, Telescope, FilterWheel, Focuser, Rotator, Dome, Switch, CoverCalibrator, ObservingConditions, SafetyMonitor. These are exactly the 10 ASCOM Alpaca device types — do not invent new top-level device types.

## Language, Style, and Safety

- C++20 preferred, RAII, small focused functions.
- Use `#pragma once` in headers.
- Prefer `enum class`, `std::chrono`, `std::string_view` where appropriate.
- No `using namespace std;` in headers.
- Core/driver layers should avoid heavy framework dependencies.
- License headers must remain AGPL-3.0-or-later and unmodified in all source files.

## Units and Behavior Conventions

- Exposure: seconds
- Angles: degrees
- RA: hours
- Dec: degrees
- Pixel size: microns
- Time: UTC with `std::chrono`

## Target Architecture

- **Linux arm64 only** (ARMv8 — Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate). amd64/x86_64 is no longer supported, built, packaged, or validated. CMake, `debian/rules`, `build_and_run.sh`, and `install_alpaca_service.sh` all hard-fail on non-arm64 hosts.
- When writing driver code, follow fixed-width integer practices for protocol/SDK structs (`int32_t`, `uint16_t`, etc.) and avoid `long double`. The wider portability concerns (endianness, alignment) no longer matter for our build target, but using fixed-width types still makes wire-protocol code easier to read and harder to misread.
- ConformU validation is performed on arm64 only. Historical amd64/x64 ConformU reports have been deleted from `AlpacaCore/conformu/`.

## Driver Implementation Rules

- Use 3-layer driver pattern:
  1. Alpaca interface (`include/alpacacore/*_driver.h`)
  2. Vendor wrapper (`include/alpacacore/vendor/<vendor>/...`)
  3. Vendor implementation (`src/vendors/<vendor>/...`)
- Do not include raw vendor SDK headers outside wrapper implementation files.
- Convert vendor failures to `AlpacaException`.
- Gold-standard runtime semantics for drivers:
  - Async `connect()/disconnect()` with `get_connecting()`.
  - Synchronous `set_connected()` for compatibility.
  - Useful `get_device_state()` telemetry.
  - Clean thread/task shutdown in destructors — **[Driver concurrency &
    lifecycle](#driver-concurrency--lifecycle-read-before-writing-a-driver) is the
    single most important section in this file; every rule there was learned from a
    review round.**
- Add TODO comments where vendor protocol/SDK behavior is uncertain.

### Driver concurrency & lifecycle (read before writing a driver)

**Apply this checklist up front.** ConformU is single-threaded and catches *none*
of the races below — code review plus the TSan concurrency stress suite do
(`[stress]` tests under the `sanitizers-tsan` CI job / `RUN_TSAN=1` pre-flight,
issue #101); a miss that neither catches becomes a review round. The rules are
vendor-agnostic; do them in the driver from the start.

**Threads & shutdown**
- Async connect: inherit the shared base —
  `class FooDriver : public XDriver, protected alpacacore::AsyncConnectable`
  (`<alpacacore/async_connectable.h>`, issue #100). It owns the connection
  thread, the `shutting_down_` destructor guard, and the never-drop-a-racing-
  disconnect protocol (pending-disconnect record/consume + Idle-published-
  under-the-lock tail). **Do not hand-roll `start_connection_task` /
  `connection_thread_` / `connecting_` in a driver.** The driver obligations
  (each one line, all contractual — see the header comment): destructor calls
  `shutdown_connection()` FIRST; `connect()`/`disconnect()` forward to
  `start_connection_task(true/false)`; `get_connecting()` returns
  `connection_task_active()`; `set_connected` gates with
  `record_disconnect_if_connect_in_flight(...)` / `consume_pending_disconnect()`
  after taking the driver mutex, before the idempotency early-return. A driver
  with an extra sync-connect window the base can't see (e.g. the AFW's
  mutex-released homing poll) records it itself via
  `record_pending_disconnect()`.
- **Never `.detach()` a thread that touches `this`.** A `sleep_for` timer that later
  writes a member (e.g. a pulse-guide flag) is the classic trap: if the object dies
  mid-sleep the wakeup writes freed memory (UB). Make it a joinable member thread
  with a cancel flag + `std::condition_variable`, and cancel + join it in the
  destructor. **The 2026-07 full-codebase audit found this rule held on every
  connect/disconnect *lifecycle* path but was violated on a dozen *operational*
  paths** (async slews, GOTO setup, pulse-guide timers, cooler-off, slew-completion
  tails) — apply the checklist to every thread a driver spawns, not just the
  connection machinery. Two corollaries from the same audit: an async tail that
  calls anything throwing (e.g. a `*_locked()` helper that rethrows `NotConnected`)
  must be wrapped in try/catch inside the lambda or it `std::terminate`s the whole
  server; and a `joinable()` pre-check before spawning a shared member thread is a
  double-start race unless the check+spawn+assign runs under one mutex.
- The destructor joins **every** background thread (connection, exposure, timers)
  before members are destroyed.

**Handles, locks & disconnect — the #1 source of use-after-close bugs**
- One fixed lock order everywhere: `driver mutex_` → operation lock
  (`readout_mutex_`, …) → SDK-wrapper mutex. Never acquire in reverse.
- An SDK call on a closable handle is safe only if the handle can't be closed
  underneath it. Two valid shapes: **(a)** hold the driver `mutex_` across the whole
  SDK call (fine for fast, non-blocking calls); **(b)** if you snapshot the handle
  and then use a separate op-lock, `set_connected(false)` must take that op-lock
  (after `mutex_`) across the close, **and** the operation must re-check
  `ensure_connected()`/`!handle_` under the op-lock before the SDK call. A
  snapshot-then-call gap with no re-check is a use-after-close.
- **A "copy the handle out, then call the SDK" helper is a trap** — returning the
  handle from a locked getter and calling the SDK *after* the lock releases is
  exactly the snapshot-then-call gap. Prefer a `with_handle([&](h){ return
  sdk.foo(h); })`-style helper that holds `mutex_` **across** the SDK call (shape
  (a)) for every fast option read/write, so there's no window at all. Reviewers
  will flag these one method at a time; convert the whole class at once. The
  ToupTek camera driver is the reference (`with_handle`); the ZWO, Player One,
  and SVBONY cameras use the same shape as `with_camera` (issue #116). Only the
  exposure worker keeps a bare snapshot — it must not hold `mutex_` across a
  blocking image wait (`WaitImageV4`, `SVBGetVideoData`, `POAGetImageData`),
  and its close is stop-and-joined first. Same exemption for a call that
  blocks for its whole duration on the device (SVBONY `SVBPulseGuide`).
  A helper returning a **reference** into a locked container (`control_caps_`)
  is the same trap one level up — return by value.
- `set_connected(false)` clears driver state (`connected_`, handle, cached info,
  element/name containers) **before** the SDK close, so a throwing close can't trap
  the driver half-connected. A getter that checks `connected_` and then re-locks to
  index a container disconnect clears has a TOCTOU — re-assert the connection under
  the lock before indexing.
- **Connect side (mirror of the above):** once you've opened the handle, guard the
  **entire** remaining init so any throw closes it before returning. If `connected_`
  is only set true at the very end, the destructor's `if (connected_)` close won't
  fire — and with a **ref-counted** open (`open_count` stays at 1) the leak is
  permanent: the next reconnect bumps the count to 2, hands back the same stale
  handle, and `Close` never balances. Don't leave post-open SDK calls
  (`put_trigger_mode`, `get_serial_number`, …) outside the cleanup try.
- **A blocking SDK call with no timeout of its own can hang disconnect forever
  — bound the wait, detach on timeout, and reference-count the handle** (QHY
  ConformU session, 2026-07). Some vendor SDK calls (QHY `ControlQHYCCDTemp`,
  a PID loop documented at ~10s but occasionally much longer; `SetQHYCCDParam`
  on some control IDs) have no cancellation and no SDK-side timeout. If a
  background worker (temp-control thread, cooler-off task) is stuck inside one
  when disconnect wants to join it, an unbounded `join()` hangs disconnect —
  and every ASCOM client (ConformU included) applies its own ~5s budget to the
  bare `Disconnect()` method, so "just wait longer" is not an option. Fix
  shape: (1) give the worker's own "is it still running" flag as a
  `shared_ptr<std::atomic<bool>>` (same pattern as a detached timer's flag,
  Threads & shutdown above); (2) bound the join with a short deadline (2s —
  comfortably under the ~5s client budget) and `.detach()` instead of
  `.join()` on timeout; (3) make that detach *safe* by reference-counting the
  SDK handle itself (`shared_ptr<qhyccd_handle>` with a
  `CloseQHYCCD`-on-last-reference deleter in the wrapper, not a raw pointer)
  so a concurrent `close_camera()` can never invalidate a handle the detached
  worker is still using — the physical close is deferred until every in-flight
  call actually finishes, instead of racing it. Same shape applies to any
  vendor SDK with a long, uncancellable, no-timeout call.

**Long-op / exposure state machines (cameras)**
- A runtime register write during a live exposure corrupts the frame. Guard it with
  an `exposure_active_` flag, checked **under the same lock that publishes it**
  (`start_exposure` sets it under `readout_mutex_`; the setters check it under
  `readout_mutex_`). Checking the flag *before* taking that lock is a TOCTOU.
- Abort/stop must **wake a blocking SDK wait** (call the SDK's stop/cancel) before
  joining the worker — setting a flag alone makes `join()` block for the whole
  remaining operation (a 10-min frame → a 10-min abort).
- Don't clear pending/dirty flags before validation that can throw; clear them at
  the *end* of the locked snapshot block, after the throwing validation.

**ASCOM contract precedence (ConformU enforces this)**
- Parameter/range validation (`InvalidValue`) precedes the connection check — an
  out-of-range id/index is `InvalidValue` even while disconnected. Every property
  otherwise throws `NotConnected` when disconnected (no early-return that skips it).

**Config round-trip (silent data loss on save)**
- Every persisted field allowlisted per device type in `sanitize_device_config`, every
  non-ZWO form field `name` vendor-prefixed — the full rules live in ONE place:
  [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors).
  The round-trip test (Required Test Case #6) is the automated catch.

> The connection-thread lifecycle lives in ONE place: `AsyncConnectable`
> (`AlpacaCore/include/alpacacore/async_connectable.h`). Every vendor driver
> inherits it (issue #100); a new driver that copy-pastes its own
> `connection_thread_` machinery is a review-blocking regression.

Two of our worst deadlocks are documented later, not in the checklist above — read
[`disconnect_locked()`](#reconnect-must-not-self-deadlock-disconnect_locked) and the
narrow-`firmware_mutex_` rule (under "Device firmware / SDK version") before touching
any connect/disconnect path or a getter that takes the coarse driver `mutex_`.

### Review bot on fork PRs (`safe-to-review` label)

The Claude review bot (`.github/workflows/claude-review.yml`) runs automatically on
every push to a same-repo PR branch. Fork PRs are gated: the bot runs only while a
maintainer has applied the **`safe-to-review`** label — applying it triggers the
first review immediately, later pushes keep reviewing while the label stays on, and
removing the label stops the bot. The label is the maintainer's trust decision: the
workflow runs with `pull_request_target` (definition always taken from `main`, so a
fork can't alter the bot's prompt/tools), and the residual risk of the bot *reading*
hostile PR content is accepted per-PR by whoever applies the label.

### When fixing a review finding (avoid the regression treadmill)

Across our driver PRs, most review rounds were spent on **regressions introduced by
the previous round's fix**, not new bugs. Before pushing any fix:

- **Sweep the symmetry.** A fix almost always has mirror sites that need the same
  change in the same commit: getter ↔ setter, `open` ↔ `close`, `connect` ↔
  `disconnect`, POSIX ↔ Windows, and every sibling accessor that shares the
  invariant. Nearly every regression we shipped was "fixed one of N."
- **MANDATORY before pushing any fix — write out the sibling set.** The bullet above
  is not advisory; a review round spent re-flagging the mirror of the fix you just
  pushed is a *process failure*, not a new bug. Before every push, state explicitly
  (in the commit body or PR comment) the full set of sites that share this defect's
  shape and confirm each is fixed **in this same commit** or is genuinely N/A. Do not
  push a fix for one member of a pair/family and "wait to see" if the reviewer flags
  the rest — grep for them yourself first. Concrete misses this cost us on the ToupTek
  AFW PR (#99), each an avoidable extra round:
  - Fixed `set_readout_mode`'s pre-lock spec/handle TOCTOU, pushed, **then** the bot
    flagged the identical bug in `get_readout_mode` the next round. Getter/setter pair —
    should have been one commit.
  - Fixed the *sync* `set_connected(false)` dropped-disconnect-during-homing, pushed,
    **then** the bot flagged the *async* `disconnect()` → `start_connection_task(false)`
    route with the same drop. Both disconnect entry points share the flag — should have
    been one commit.
  When you touch one enumerator/getter/setter/entry-point, `grep` the sibling family
  (`enumerate_*`, `get_*`/`set_*` for the same property, every `disconnect` route the
  router can dispatch) and fix or dismiss each **before** the push, naming them in the
  writeup so the sweep is auditable.
- **A new invariant must be applied everywhere it is read/written, at once.** If a fix
  establishes "X only changes under lock L" (e.g. `exposure_active_` under
  `readout_mutex_`), grep every read and write of X and bring them all under L in the
  same change — a partially-applied invariant is worse than none.
- **Re-run the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver)
  over the changed lines *and their siblings*** each round, not just at authoring.
- **Verify a suggested fix before applying it verbatim** — even the reviewer's; one
  bot-recommended race fix was itself a use-after-close.
- **"Approved" is not a final stop signal.** The review bot is non-deterministic and
  has re-opened PRs it approved. Treat *"no confirmed bugs + ConformU 0/0/0 + all
  gates green"* as the merge bar, not a literal zero-finding run.

### ASCOM exception vocabulary (pick the right one — ConformU checks it)

| Throw | When |
|---|---|
| `InvalidValue` | Bad argument / out-of-range id or index — **even while disconnected** (precedes the connection check). |
| `NotConnected` | Any operational property/method called while disconnected. |
| `PropertyNotImplemented` | A property the hardware genuinely lacks (e.g. `Offsets` list, `SubExposureDuration`). |
| `MethodNotImplemented` | A method the hardware lacks (e.g. `PulseGuide` when `CanPulseGuide` is false). |
| `NotImplemented` | A generic unsupported action (e.g. `set_temp_comp(true)` with no temp-comp support) — never `DriverException` for "not supported". |
| `InvalidOperation` | Valid call, wrong state (e.g. changing readout mode/geometry mid-exposure). |
| `DriverException` | A genuine internal/driver failure only — not a stand-in for any of the above. |

All map to HTTP 200 with a non-zero `ErrorNumber` — clients read the body, not the status.

### Serial / socket I/O: always use the shared helpers (`util/serial_io.h`)

POSIX serial and socket code has several easy-to-get-wrong patterns that must not
be hand-rolled in a wrapper. Use `alpacacore/util/serial_io.h` (POSIX-only,
included inside the existing `#ifndef _WIN32` branches):

- **`util::write_all(fd, data, len)`** instead of a bare `write()`. `write()` may
  satisfy only part of the payload (`0 < n < len`) or be interrupted (`EINTR`);
  treating any non-negative return as success silently drops trailing bytes (e.g.
  a command terminator). `write_all` loops until the whole payload is written and
  treats a `0` return as a hard error (no infinite spin).
- **`util::send_all(fd, data, len, MSG_NOSIGNAL)`** for every socket `send()`. The
  socket analogue of `write_all`; **always pass `MSG_NOSIGNAL`** so a peer drop
  mid-send returns an error instead of delivering `SIGPIPE` (which would kill the
  server). A short send that isn't completed corrupts the next command's framing.
- **`util::clear_nonblocking(fd)`** / **`util::set_nonblocking(fd)`** instead of a
  raw `fcntl(F_GETFL)`+`F_SETFL`. A failed `F_GETFL` returns `-1`; feeding that into
  `F_SETFL` can leave the fd in the wrong mode (a stuck-non-blocking fd spins a
  reader at 100% CPU; a stuck-blocking connect socket hangs for the full ~127s TCP
  timeout). Both helpers check both `fcntl` calls and return false on failure.

Apply these on **both** the auto-detect probe path **and** the production
connect/open path — the fd is typically opened `O_NONBLOCK`, so `connect_serial()`
must `clear_nonblocking()` after `tcsetattr` (not just the probe), or reads ignore
`VMIN`/`VTIME` and `write_all` fails on `EAGAIN`. Use the **same abort-on-failure
pattern** (`close(fd); return false/""`) at every call site.

### Camera ROI alignment (all camera vendors)

Every camera SDK constrains ROI geometry, and the pattern is the same everywhere:
**keep the client-requested values for the Alpaca interface, align down for the SDK
call, and pad outputs if needed** — ConformU's read-back checks must see the requested
geometry. Per-SDK constraints (the only vendor-specific part): ZWO width%8 / height%2
after binning; SVBONY width%8 / height%2; Player One width%4 / height%2; ToupTek even
sensor-resolution width/height/offset (see the ToupTek odd-bin-factor note for the
3×3 subtlety).

### FilterWheel semantics (all vendors)

- **`Position == -1` IS the ASCOM "moving" sentinel.** SDKs that report `-1` while in
  motion (ToupTek AFW) or a distinct moving state (Player One `PW_ERROR_IS_MOVING`)
  map directly onto it — pass it through; don't invent a separate is-moving flag, and
  never translate the SDK's moving-read into an exception on the read path.
- **Names must be non-empty** — default `"Filter 1..N"`; names and focus offsets are
  settable while disconnected.
- **DeviceState includes operational fields only** (e.g. `Position`); omit `Connected`
  for ConformU compatibility.

### GPIO power-switch / soft-PWM drivers (general rules)

All GPIO 12V power-port Switch drivers (ZWO ASIAIR Pro / Plus CM4 / Plus RK3568,
ToupTek StellaVita, iOptron iMate, and any future board) share these rules; the vendor
notes carry only the pin map and per-board deltas.

- **libgpiod v2 only** (`libgpiod-dev (>= 2.0)`, `libgpiod3` runtime): one
  `gpiod_line_request*` owns all lines together; values go through
  `gpiod_line_request_set_value`. Never port back to the v1 per-line API. The daemon
  user needs `gpio`-group access to the chip/char device via a udev rule.
- **Boot-high preserve**: these boards drive the DC ports HIGH at boot
  (`gpio=...=op,dh[,pu]`), so attached gear is powered before userspace runs. The
  wrapper requests lines with an initial value of high and defaults its cached state
  to "on" — connecting the driver must not glitch power.
- **Never power-cycle on disconnect**: `close()` releases the lines without driving a
  boolean line low; a PWM port first stops its worker and drives a defined steady
  level (duty > 0 ⇒ high). Users who want a port off must set it off in the client
  before disconnecting. Do NOT add a drive-low-on-close path without making it opt-in
  config — it would silently flip the policy for everyone who upgrades. Documented
  user-facing in [`AlpacaCore/PowerPorts.md`](AlpacaCore/PowerPorts.md).
- **Userspace soft-PWM, per-port worker threads** (`sleep_until` bit-bang): hardware
  or DMA PWM is board-specific and unavailable/unreachable on every board we ship, so
  userspace toggling is the standard mechanism. Steady-state 0%/100% skips the
  per-period syscall. Every line write checks the return code — on failure, log at
  ERROR and set the per-port stop flag so the thread exits cleanly instead of looping
  while the ASCOM API reports success. Two-phase shutdown: signal stop under the
  mutex, join outside it, release/close back under it (the classic
  join-under-the-same-mutex deadlock otherwise).
- **PWM frequency is the lever, not load type** — a flat panel's internal LED driver
  smooths a too-fast chop into plain on/off (a panel that "won't dim" at 1 kHz dims
  fine at 50 Hz); resistive dew heaters dim at any frequency; regulated gear
  (cameras/mounts) stays on/off regardless. The default is **per-driver**, verified
  against the stock firmware's actual value or on real hardware — never re-derived
  from bench psychoacoustics: ASIAIR Pro/CM4 1 kHz, ASIAIR Plus RK3568 50 Hz, iMate
  50 Hz, StellaVita 100 Hz.
- **Read-only pass-through ports** (e.g. iMate DC3): writes throw `NotImplemented`
  *before* the connection check, so the static capability holds while disconnected
  and is unit-testable without hardware.

### Device firmware / SDK version: web UI only, never `DriverInfo`

Two **separate** optional hooks, both default `std::nullopt`:

- `AlpacaDriver::get_device_firmware()` — the **device's own hardware firmware**
  (a mount/handset firmware version, a camera's on-board firmware, etc.).
- `AlpacaDriver::get_device_sdk_version()` — the **vendor SDK/library version**
  the driver links against (a host software version, not a hardware property).

Keep them distinct — do NOT report an SDK version from `get_device_firmware()`;
that mislabels a library version as firmware (e.g. ZWO's ASI SDK has no device-
firmware API, so a ZWO camera reports only `get_device_sdk_version()`). The
management `configureddevices` response adds per-device `Firmware` / `SdkVersion`
fields only when the connected driver returns each value, and the web UI renders a
"Firmware" / "SDK Version" row only when present. **Neither goes in the ASCOM
`DriverInfo` string** — it stays clean for NINA / other Alpaca clients (the QHY
camera's pre-existing SDK-in-`DriverInfo` is grandfathered; do not copy it).
`DriverVersion` always stays the AlpacaBridge software version. Both hooks must be
cheap and non-blocking: protocol drivers whose firmware getter does live serial
I/O must cache the value at connect and return the cached copy; SDK drivers return
the static SDK version directly. Where a vendor SDK exposes both (e.g. SVBONY:
`SVBGetCameraFirmwareVersion` for firmware), report each via its own hook. Return
`std::nullopt` when disconnected and the value is unknown.

**Guard the cache with a DEDICATED narrow mutex, never the coarse driver
`mutex_`.** `set_connected()` typically holds the driver `mutex_` across the
entire multi-second connect (SDK open, serial handshake, site/time sync). If
`get_device_firmware()` takes that same `mutex_`, a `/management/v1/configureddevices`
poll arriving mid-connect blocks the HTTP thread for the whole connect. Add a
separate `firmware_mutex_` + `firmware_cache_`, populate it at connect and clear
it at disconnect (both under `firmware_mutex_`), and read only it from the getter
(WandererCover caches in the protocol wrapper; Gemini/SVBONY/SynScan/Celestron use
a `firmware_mutex_`). Do NOT consult `connected_` in the getter — rely on the
cache being empty while disconnected, so there is no atomic-vs-mutex ordering bug.

### Reconnect must not self-deadlock: `disconnect_locked()`

A protocol wrapper's `connect()` that re-uses an existing connection typically
does `lock(mutex_); if (connected_) disconnect();` — but if `disconnect()` also
locks `mutex_`, the non-recursive `std::mutex` **deadlocks** on reconnect (user
changes a port and clicks Connect while connected → hangs the connection thread →
hangs the server). Split it: a `disconnect_locked()` with the teardown body and NO
lock (caller must already hold `mutex_`), and a public `disconnect()` that locks
and delegates. `connect()` calls `disconnect_locked()`; external callers call
`disconnect()`. All protocol wrappers follow this (gemini/ioptron/synscan/
celestron/bisque/zwo-mount).

### Auto-detect failure message (`util/auto_detect.h`)

Serial port enumeration is POSIX-only, so the `enumerate_*_ports()` helpers return
empty on Windows. When an auto-detect driver finds no ports, throw
`util::serial_auto_detect_failed_message("<device label>")` rather than a
hard-coded "no device found" string, so the Windows path reports "auto-detect not
supported on this platform" instead of implying missing hardware.

### Platform 7 InterfaceVersion + DeviceState

- Drivers advertise ASCOM Platform 7 interface versions: Camera 4 (ICameraV4),
  Telescope 4, Focuser 4, Rotator 4, FilterWheel 3, Switch 3, ObservingConditions 2.
  Keep `get_interface_version()` and its unit-test assertion in sync when adding a driver.
- **Do not** write a per-vendor `get_device_state()`. Each device base class
  (`CameraDriver`, `TelescopeDriver`, …) implements it once, inline, building the
  operational-property list by calling that device's own property getters inside a
  `try { … } catch (const std::exception&) {}` (a getter that throws — `AlpacaException`
  or any unwrapped vendor error — is omitted, never propagated) and
  appending a `TimeStamp` via the inline `device_state_timestamp()` helper. Using the
  same getters as the GET endpoints guarantees DeviceState ↔ GET consistency, which is
  what ConformU checks. A new vendor driver inherits the compliant DeviceState for free.
- DeviceState is **not an atomic snapshot**: each getter locks the driver mutex
  separately, so e.g. `RightAscension` and `Slewing` can straddle a state change, and a
  device dropping mid-call yields a partially populated response. The old per-vendor
  overrides read everything under one mutex. ASCOM doesn't require atomicity and
  ConformU only checks DeviceState ↔ GET consistency, so don't "fix" this by adding a
  snapshot lock — but don't build features that assume mutual consistency within one
  DeviceState response either.
- The base `get_device_state()` and `device_state_timestamp()` are **inline in the
  headers on purpose**: an out-of-line virtual would make the device class's vtable a
  "key function" emitted only in the core library, and the per-vendor static libraries
  (linked before it) would fail to resolve `vtable for XDriver`. Keep them inline.
- ConformU is lenient about DeviceState contents (it does not require a fixed property
  set or even a TimeStamp — the iOptron switch passed at ISwitchV3 with neither), but it
  does flag values inconsistent with the individual GETs. The getter-based pattern above
  satisfies it. Still, bumping any InterfaceVersion **requires a fresh ConformU V4 run on
  real hardware** before release, since it switches ConformU to the stricter test suite.

## CMake and Vendor Integration

- Guard each vendor behind explicit build options.
- When adding a vendor in `AlpacaCore/CMakeLists.txt`, always update:
  1. `option(ALPACACORE_ENABLE_<VENDOR> ...)`
  2. `ALPACACORE_ENABLE_ALL_VENDORS` logic (only if implemented)
  3. conditional `add_subdirectory(src/vendors/<vendor>)` + link
  4. install rules for vendor target
- If vendor libs are discovered by pkg-config, prefer imported targets (example: `PkgConfig::LIBUSB`) so dependent test binaries get correct link paths.
- When adding a new vendor SDK under `AlpacaCore/external/<vendor>/`, add an allowlist entry to `AlpacaCore/.gitignore` so the SDK binaries (`.a`, `.so`, `.dll`, firmware files, etc.) are not blocked by the global compiled-file ignore rules. Follow the existing pattern: `!external/<VENDOR>/**`.

## Vendor SDK Shared Library Packaging

**Every new camera vendor MUST ship its `.so` in the `.deb`, regardless of whether AlpacaBridge itself statically links the library.** Camera `.so` files are also consumed by companion projects (e.g. SmartGuider at `/home/dev/Documents/GitHub/SmartGuider/` — uses `libASICamera2.so` via `zwoasi`, `libqhyccd.so`, etc. for autoguiding) which dynamically `dlopen` them from the system library path. If the `.so` is missing or not registered with `ldconfig`, guiding fails at runtime with no warning from the AlpacaBridge build or test suite. The fact that the AlpacaBridge server binary links fine is NOT evidence that packaging is correct.

Non-camera vendors (focusers, mounts, switches, rotators) also ship their `.so` if the SDK provides one, for consistency — but the camera rule is non-negotiable.

### Mandatory Checklist — New Vendor SDK with `.so`

Do **all** of the following when adding a new vendor SDK. Skipping any step will either silently break companion projects (step 1), break CI / clean clones (step 2), or break runtime loading on installed systems (steps 3–5).

1. **Store** static (`.a`) and shared (`.so`) libraries under `AlpacaCore/external/<VENDOR>/` in the arm64 subdirectory the upstream SDK uses (commonly `lib/linux/armv8/`, `lib/linux/arm64/`, or `lib/armv8/`). Document the exact path in the vendor-specific notes section below. Do not commit x86_64/x64 SDK binaries — AlpacaBridge is arm64-only and they would only bloat the repo.
2. **Allowlist in `.gitignore`**: add `!external/<VENDOR>/**` to `AlpacaCore/.gitignore` **before** committing the SDK files. The global `*.so` ignore rule will silently drop the library from the commit otherwise. Verify with `git check-ignore -v <path-to-.so>` — the output must show the `!external/<VENDOR>/**` rule winning.
3. **`debian/rules`** — copy `.so*` to `$(STAGING)/usr/lib/alpacabridge/` in `override_dh_auto_install`, alongside existing QHY/ZWO/SVBONY entries. Add a `<VENDOR>_LIB_DIR` variable at the top of the file pointing at the arm64 SDK path.
4. **`build_and_run.sh`** — copy `.so*` to `/usr/local/lib/`, run `ldconfig`. Add inside the udev rules block alongside existing QHY/ZWO/SVBONY install logic.
5. **`install_alpaca_service.sh`** — same as `build_and_run.sh`, inside the `install_udev_rules()` function. Keep the two scripts in sync — they must install the same set of vendor libraries.

### Dynamic Linker Registration

The `.deb` ships `/etc/ld.so.conf.d/alpacabridge.conf` which adds `/usr/lib/alpacabridge` to the system library search path. The `postinst` script runs `ldconfig` so libraries are discoverable immediately after install. This is what makes companion projects (SmartGuider's `zwoasi`, `ctypes.CDLL('libtoupcam.so')`, etc.) able to find vendor libraries without setting `LD_LIBRARY_PATH`.

### SDK Version Bumps

When a vendor releases a new SDK version, update the `.so` files in `external/` and bump symlink targets (e.g. `libASICamera2.so.1.41` → `libASICamera2.so.1.42`). If the vendor's SDK path is versioned (e.g. ToupTek's `toupcamsdk.20260128/`), update the path reference in `debian/rules`, `build_and_run.sh`, `install_alpaca_service.sh`, AND `AlpacaCore/src/vendors/<vendor>/CMakeLists.txt` in the same commit.

### Driver version (DriverVersion)

Every driver's `get_driver_version()` returns `alpacacore::kVersion` (from `<alpacacore/version.h>`), which is the single workspace `VERSION` file injected at build time via the `ALPACACORE_VERSION` compile definition. AlpacaCore's top-level `CMakeLists.txt` sets this with directory-scoped `add_compile_definitions(...)` right after `project()`, so it reaches the core lib, every per-vendor sub-library, and the tests. **Do not hardcode a version string** in a driver. To bump the reported version for the whole project, edit the `VERSION` file only. Unit tests assert `get_driver_version() == alpacacore::kVersion` rather than a literal, so they don't need updating on a version bump.

### Verification

After wiring a new camera vendor, verify the `.so` is reachable as SmartGuider would see it:

```bash
# After ./build_and_run.sh or dpkg -i alpacabridge_*.deb:
ldconfig -p | grep <libname>                    # must list the .so
python3 -c "import ctypes; ctypes.CDLL('<libname>.so')"  # must not raise OSError
```

Failing this check means guiding will fail at runtime, no matter how green the AlpacaBridge test suite is.

## AlpacaHTTP Integration Checklist (Required for New Vendor/Device Types)

When adding a new vendor/device type in AlpacaCore, also update AlpacaHTTP:

1. **Router registration** — add vendor/device case to `Router::register_device_from_config` in `AlpacaHTTP/src/http/router.cpp`. This is the dispatch that creates driver instances from persisted JSON config.
2. **Router includes** — add `#include <alpacacore/vendor/<vendor>/<vendor>_<device>_driver.h>` at the top of `router.cpp`, guarded by `#ifdef ALPACACORE_ENABLE_<VENDOR>`.
3. **Config sanitization fields** — ensure vendor-specific config keys are preserved through sanitization.
4. **Web UI vendor dropdown** — add the vendor to the device-type dropdown in the web frontend (`AlpacaHTTP/web/app.js`) so users can select it.
5. **Web UI vendor-specific form fields** — add any vendor-specific configuration fields (e.g. serial port, camera index, connection type) to the frontend form.
6. **Web UI index fields** — if the vendor connects by an SDK enumeration index, give each index input a **unique vendor-prefixed `name`** and register it in the `INDEX_FIELDS` array so auto-numbering and manual-edit tracking work. Both are mandatory and easy to miss — see "Enumeration index fields" below for the full rationale (a generic `name` silently saves 0; a missing registry entry reuses index 0 on the next device).
7. **Frontend validation** — add any related validation logic in frontend JS.
8. **Build-flag propagation** — ensure `ALPACACORE_ENABLE_<VENDOR>` compile definitions propagate from AlpacaCore to AlpacaHTTP.
9. **Routing/config tests** — add or update tests in `AlpacaHTTP/tests/`.

Vendor registration alone is not enough for HTTP/UI visibility. All nine steps must be completed for a new vendor/device to be fully functional end-to-end.

### FilterWheel vendors — required web UI (slot count + filter name pickers)

Every filterwheel vendor's config form MUST include the standard slot UI, not just a
names textarea. It consists of: a **slot-count select listing the manufacturer's actual
wheel lineup** plus a Custom option, the per-slot filter dropdowns (LRGB/narrowband/Sloan
presets + Custom), and the "Advanced: edit filter names as lines" textarea. The whole
widget is one reusable component — instantiate `createFilterwheelSlotUI({...})` in
`AlpacaHTTP/web/app.js` with vendor-prefixed element IDs and copy the markup pattern from
an existing vendor in `index.html`. The component keeps the slot rows and the textarea in
sync; the form submit reads the textarea.

**Single-token shorthand expansion** (shared by `parseFilterNamesInput` in `app.js` and the
`normalize_slot_data_locked` in the ZWO/ToupTek filter-wheel drivers): a lone name with no
delimiters whose length equals the slot count expands to one character per slot (`LRGB` →
`L,R,G,B`). Guard it with **"no lowercase letters"** so ordinary names like `Clear` or
`Ha_NB` that happen to match the slot count are NOT exploded — a bot-review foot-gun. Keep
the JS and C++ conditions in step (both layers run the same rule).

- **When building a new filterwheel driver, ask the user what slot counts the
  manufacturer offers** and put exactly those in the select (with model names in the
  labels where known). Known lineups: ZWO EFW 5/7/8; Player One Phoenix Wheel 5/7/8
  (PW5/PW7/PW8).
- Use unique, vendor-prefixed form field `name`s (e.g. `playerOneFilterwheelIndex`,
  `playerOneFilterNames`) — the
  [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors)
  FormData-collision rule.
- When editing an existing device, populate the textarea from `config.filterNames` and
  call the instance's `syncSlotsFromTextarea()` so the dropdowns reflect the saved names.

### Enumeration index fields — unique names + auto-numbering (all vendors)

Many vendors connect by an SDK **enumeration index** (camera/focuser/filterwheel/rotator
index — "which unit on the bus", numbered from 0). Two rules keep these working; a new
vendor that ignores either ships a silently broken form:

- **Each index input MUST have a unique, vendor-prefixed `name`** — e.g.
  `playerOneCameraIndex`, `qhyCameraIndex`, `touptekFocuserIndex`, `geminiFocuserIndex` —
  and the submit handler MUST read that exact name. Hidden vendor sections are **not**
  disabled, so every index input is still in the form's `FormData`. A generic `name` like
  `cameraIndex`/`focuserIndex` collides: `formData.get('cameraIndex')` returns the **first**
  such field in DOM order (ZWO's, which appears first), so the value you typed into a
  later vendor's field is discarded and `0` is saved instead. This is exactly why a
  Player One/QHY/SVBONY/ToupTek camera index could not be changed from 0. ZWO keeps the
  bare `cameraIndex`/`focuserIndex`/`filterwheelIndex`/`rotatorIndex` names (it's the
  canonical first block); **every other vendor must prefix**. Element `id`s can stay
  descriptive (`playerone-camera-index`) — `setFormValue`/auto-fill key off `id`, the
  collision is purely about the `name` used in `FormData`. **This applies to EVERY shared
  field name, not just the `*Index` ones** — the `*Id` binding fields
  (`cameraId`/`focuserId`/`filterwheelId`/`rotatorId`) and discriminator selects
  (`switchType`) collide the same way. ToupTek's focuser-id (`touptekFocuserId`), filter-wheel-id
  (`touptekFilterwheelId`), and switch-type (`touptekSwitchType`) are all prefixed for this
  reason; a bare `name="focuserId"` on a non-ZWO input silently returns ZWO's value. When you add
  ANY input whose name matches a field ZWO already uses, prefix it and read the prefixed name in
  the submit handler.
- **Register the field in the `INDEX_FIELDS` array** in `AlpacaHTTP/web/app.js`
  (`fieldId`, `vendor`, `deviceType`, `configKey`, optional `idFieldId`). That one entry
  drives auto-increment (so a second device of the same vendor/type doesn't reuse index 0)
  **and** the manual-edit tracking. The index is scoped per `(vendor, deviceType)` — each
  SDK enumerates from 0 independently, so a ZWO camera and a Player One camera are both
  index 0. This is distinct from the Alpaca **device number** (auto-assigned per device
  type, vendor-agnostic, and what clients address). Serial/network devices (port path or
  host) have no index and belong in neither place.
- **Allowlist EVERY persisted field in `sanitize_device_config` (`router.cpp`), per device type**
  (bit us three times: ZWO ASIAIR PWM ports, ToupTek `switchType`, ToupTek AFW filter fields).
  `sanitize_device_config` is a strict allowlist — anything not explicitly `copy_if_present`-ed
  is silently dropped on save, so the config round-trips lossily and the setting reverts (a
  filter-wheel binding resets to index 0, custom filter names vanish, PWM toggles revert). When
  you add a config field that a driver reads in its registration branch, you MUST also add a
  matching `copy_if_present` in that vendor's `sanitize_device_config` branch. If a vendor
  serves multiple device types through one branch (ToupTek: camera/focuser/filterwheel/switch),
  split on `device_type` so each type keeps its own fields — don't let a shared `else` copy only
  the camera/focuser fields. Cross-check the driver's `config.value(...)` / `config.contains(...)`
  reads in the registration function against the sanitizer branch; they must list the same keys.

## Alpaca Protocol Conformance (AlpacaHTTP)

These rules come straight from the ASCOM Alpaca API definition (https://ascom-standards.org/api/) and are enforced by ConformU. Do not regress them:

- **Parameter names are case-insensitive.** The spec: "Parameter names are not case sensitive, so clients and drivers should be prepared for parameter names to be supplied ... with any casing." This applies to **both** GET query params and PUT form-body params. `Request::get_query_param`/`has_query_param` and the router's `get_form_value` all match case-insensitively. Never special-case behavior on `User-Agent` (e.g. a "strict only for ConformU" path) — test behavior must equal production behavior.
- **URLs are case-sensitive and lowercase.** Device type and method path segments must be lower-case; that check stays.
- **HTTP status codes:**
  - `200` — request was interpreted and reached the driver. Driver exceptions (NotImplemented, InvalidValue, NotConnected, etc.) ride in the JSON `ErrorNumber`/`ErrorMessage` fields with a `200`. `apply_error_status` exists to keep these at 200 — never downgrade a driver error to 4xx/5xx.
  - `400` — "the device could not interpret the request e.g. an invalid device number or misspelt device type." Use 400 (not 404) for unknown device type, unknown method, and unregistered device number. A genuinely unroutable URL (no device/management match) stays 404.
  - `500` — unexpected internal error only.
- **The Alpaca `Value` is structured JSON, never a re-parsed string.** `AlpacaResponse::value` is `std::optional<nlohmann::json>` and `to_json` emits it verbatim. Handlers assign the real type directly — scalar, string, array, or object (e.g. `alpaca_response.value = actions;` for `SupportedActions`, **not** `actions.dump()`; `make_success_response(..., gains)` where `gains` is a `nlohmann::json` array). Do NOT serialize a structured payload to a string and rely on it being re-parsed downstream. The old `to_json` ran `json::parse()` on every string `Value` and substituted the result if it parsed — which (a) corrupted scalar string properties whose text is valid JSON (`"12345"` → number, `"true"` → bool, wrong ASCOM type on the wire) and (b) forced every array/object endpoint to round-trip through `.dump()`. That heuristic bit `SupportedActions`/`DeviceState` (every device) plus camera `Gains`/`Offsets`/`ReadoutModes`, telescope `AxisRates`, and filter `Names`/`FocusOffsets` — ConformU rejected the stringified arrays ("could not be converted to IList`<String>`"). The web UI mirror (`web/app.js parseResponseValue`) only parses a string that begins with `{`/`[`, never a bare scalar. The large camera image payload uses its own `build_image_*_payload` path and never goes through `Value`.
- **The router's `Connected=false` wait must poll `get_connecting()`, never
  `get_connected()`** (QHY ConformU session, 2026-07). The `PUT /connected`
  handler synchronously waits for an async disconnect to finish before
  replying. `get_connected()` is not a valid completion signal for that wait:
  per the Handles/locks rule above, `set_connected(false)` correctly clears
  `connected_` at the *start* of teardown (so a throwing close can't leave
  the driver looking half-connected), which means `get_connected()` can read
  `false` while the disconnect task is still running. A wait that polled
  `device->get_connected() && device->get_connecting()` exited the instant
  `connected_` flipped — well before the task actually finished — so the
  handler replied "done" early, and the client's very next `Connect()` raced
  the still-running disconnect and was silently dropped by
  `AsyncConnectable`'s then-current connect-vs-in-flight-disconnect rule
  (a racing connect is now queued via `pending_connect_`, but the wait must
  still poll the right flag — an early "done" reply is wrong either way).
  `get_connecting()`
  alone is the one signal the base class guarantees stays true for a task's
  entire lifetime, across every driver that inherits it — see
  `test_async_connectable.cpp` for the regression test. This is a router bug,
  not a driver bug: no per-driver fix can work around a caller that trusts
  the wrong flag.
- **`Connected` is per-client, refcounted in the router — never wire an
  endpoint straight to `device->connect()`/`disconnect()`** (issue #160).
  Alpaca is designed for several clients sharing one device (imaging app +
  guider on the same mount), so the router keeps a per-device registry of
  connected ClientIDs (`Router::register_client_connection` and friends):
  first client in powers the upstream link, `PUT connected=false` (and
  Platform 7 `disconnect`) only tears it down when the LAST registered
  client leaves, and `GET connected` answers the *caller's* registration
  AND-ed with device state (ClientID-less requests share one anonymous slot
  and read raw device state on GET). Supporting rules: a dead upstream link
  (`!get_connected() && !get_connecting()`) clears the whole registry so
  every client observes the failure; a failed connect drops the caller's
  registration; any request from a client refreshes its registration, and
  registrations idle >10 min expire so vanished clients can't pin the
  device connected; device removal clears the registry entry (the map is
  keyed by driver pointer — a later driver at a recycled address must not
  inherit registrations). If you add any new endpoint that connects or
  disconnects a device, route the decision through this registry.
- Regression tests for the above live in `AlpacaHTTP/tests/test_routing.cpp` and run vendor-free.

## Debian Packaging

- Package files live in `debian/` (control, rules, copyright, service file, maintainer scripts).
- **`debian/changelog` is generated, never edited.** It is untracked/gitignored and derived from the root `CHANGELOG.md` by `scripts/changelog_to_deb.py` (same design as the OpenAstro Guider). Build the package with `scripts/build_deb.sh`, which generates the changelog (version from the `VERSION` file, validated with `dpkg-parsechangelog`) and then runs `dpkg-buildpackage -us -uc -b`. Do not run `dpkg-buildpackage` directly on a fresh checkout — it will fail on the missing `debian/changelog`. The in-progress CHANGELOG section uses this repo's `## [X.Y.Z] - UNRELEASED` convention; the generator synthesizes an `UNRELEASED` stanza from it when `VERSION` has not been released yet, and warns when `VERSION` and the section label disagree.
- The `.deb` installs to:
  - `/usr/bin/alpacabridge` — server binary.
  - `/usr/lib/alpacabridge/` — vendor shared libraries (e.g. `libqhyccd.so`, `libASICamera2.so`).
  - `/usr/share/alpacabridge/web/` — web UI static files.
  - `/lib/firmware/qhy/` — QHY camera firmware files.
  - `/lib/udev/rules.d/` — udev rules for USB device permissions.
  - `/usr/sbin/fxload` — QHY firmware loader.
  - `/etc/alpacabridge/` — default config (`registered_devices.json`).
- When adding a new vendor with shared libraries, update `debian/rules` `override_dh_auto_install` to copy them into `$(STAGING)/usr/lib/alpacabridge/`.
- To cut a release, bump the `VERSION` file and date the `## [X.Y.Z]` CHANGELOG.md heading — **do NOT edit `debian/changelog`; it is generated** (see the packaging note above).

### Version bump policy (SemVer)

`VERSION` and the `## [X.Y.Z] - UNRELEASED` CHANGELOG heading move together, per
SemVer: **new driver/feature = minor bump; fix- or docs-only = patch; breaking change
(dropped platform, config-schema break) = major.** The UNRELEASED section carries
forward cumulatively until release — if it already sits at a minor bump and another
driver lands, the number stays; a feature landing on a patch-level UNRELEASED raises
it to the next minor. `/commit` and `/submit-pr` enforce this; it is documented here
so a driver-building agent bumps correctly without them.

## Testing Requirements

- Non-trivial code must have unit tests under `AlpacaCore/tests/` or `AlpacaHTTP/tests/`.
- Build driver targets and test targets together.
- Tests should be runnable via `run_all_tests.sh`.
- Preferred test naming:
  - `test_<component>.cpp`
  - `test_<vendor>_<device>.cpp`
- Use tags to separate unit/integration/hardware behavior when applicable.
- Use Catch2 macros (`REQUIRE`, `CHECK`, `CHECK_THROWS_AS`, etc.) via the `catch2_compat.h` header.
- **AlpacaHTTP hand-rolled tests must not use `assert()`.** The AlpacaHTTP tests (`test_routing`, `test_json`, `test_config`, `test_discovery`) are plain `int main()` programs, not Catch2. They use the always-on `EXPECT()` macro from `AlpacaHTTP/tests/test_assert.h`. Never use `<cassert>` `assert()` there: it is compiled out under `-DNDEBUG` — which Release, `debian/rules`, and the shipped `.deb` all define — so an assert-based check silently does nothing in an optimized build. Worse, an `assert(side_effecting_call())` (e.g. `assert(request.parse(...))`) means the call itself never runs under `NDEBUG`, so the test exercises nothing and can crash on the resulting empty state. `run_all_tests.sh` and CI build *without* `NDEBUG`, so this class of bug hides until someone builds Release. `EXPECT()` evaluates its expression exactly once and aborts on failure regardless of build type.

### Required Test Cases for Every New Vendor Device Driver

Every new driver **must** ship with at least the following test cases. Use the existing tests (e.g. `test_svbony_camera.cpp`, `test_gemini_focuser.cpp`) as reference.

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
   - `CHECK_THROWS_AS` for `action()`, `command_blind()`, `command_bool()`, `command_string()`.

5. **Device-specific behavior** — at least one test covering behavior unique to the device type:
   - Cameras: sub-exposure support (`get_sub_exposure_duration` / `set_sub_exposure_duration` throw if unsupported).
   - Telescopes: target coordinate validation, axis rate ranges.
   - Focusers: `get_absolute`, `get_temp_comp_available`, device state telemetry.
   - Filter wheels: names/offsets defaults, invalid position handling.
   - Switches: `get_max_switch`, invalid switch ID handling.
   - Rotators: `get_can_reverse`, device state telemetry.

6. **Config save→load round-trip** in `AlpacaHTTP/tests/test_routing.cpp` — `configuredevice` then read back `configureddevices` and assert **every persisted field survives** (index/id, filter names, PWM/port config, etc.). The automated catch for the two silent-data-loss classes described in [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors). Model it on the existing ToupTek AFW filter-wheel round-trip test.

### Hardware-free driver tests via the SDK seam (ToupTek pattern — extend to other vendors)

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
  the reusable pattern from issue #104.
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

### Test CMake Integration

When adding a test file for a new vendor device:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list in `AlpacaCore/tests/CMakeLists.txt`, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.
- Build and run all tests (`cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests`) before considering the driver complete.

## Continuous Integration and Pre-flight

- CI (`.github/workflows/ci.yml`) runs on every PR, all on the native arm64 runner: `build-test` (vendors OFF) + `build-vendors` (vendors ON), `sanitizers` (ASan+UBSan), `sanitizers-tsan` (ThreadSanitizer over the `[stress]` connect/disconnect/operate concurrency suite, all vendors ON), `clang-format`, `clang-tidy`, `cppcheck`, `unicode`, `shellcheck`, `javascript`, and `zizmor`.
- **Run `scripts/ci_preflight.sh` before opening a PR** (it is the `/submit-pr` Step 4 hard gate). It reproduces the CI gates locally, auto-installing missing tools, and exits non-zero if any mandatory gate fails — catching failures before they ever reach CI.
- **cppcheck is pinned to 2.17.x, built from source in CI.** The `ubuntu-24.04-arm` runner's apt cppcheck is 2.13, which classifies some checks differently from the 2.17 on a Debian Trixie dev box (e.g. `virtualCallInConstructor` is a `warning` in 2.13 but reclassified in 2.17). Since `ci_preflight.sh` runs whatever cppcheck the dev box has, that version skew let the local pre-flight and CI disagree. Building 2.17 from source (checksum-verified, mirroring the libgpiod-from-source step) keeps them aligned. **Keep the cppcheck `--suppress` list identical between `ci.yml` and `ci_preflight.sh`.**
- **Web UI JavaScript is gated only by `node --check`** (the `javascript` job + pre-flight gate). The web UI is hand-written static JS with no bundler/eslint/`package.json`, so this parse-only check is its sole automated validation — there is nothing else stopping a stray brace from shipping.
- `zizmor`'s pinned version + sha256 appear in both `ci.yml` and `ci_preflight.sh` — bump them together.
- **Concurrency now has automated coverage — but only where a driver is registered with the stress harness.** The `sanitizers-tsan` job (issue #101) builds all-vendors with ThreadSanitizer and runs the `[stress]` connect/disconnect/operate suite (`AlpacaCore/tests/concurrency_stress.h`): lifecycle storms from N threads, destruction racing an in-flight connect, and the racing-disconnect-never-dropped settle check. Locally: `RUN_TSAN=1 ./scripts/ci_preflight.sh`. Registered so far: ToupTek camera / AFW / thermal switch (over the fake SDK seam, wrapped in `LockedToupTekSDK`), ZWO EFW + camera, Player One Phoenix + camera, SVBONY camera, Bisque, and — over the loopback fake-mount TCP seam (`tests/fake_mount_server.h`, which drives drivers into the *connected* state so the poll/pulse/GOTO/teardown threads actually run) — the ZWO, Celestron, SynScan, and iOptron telescopes. **When you add or substantially change a driver, add a `[stress]` TEST_CASE for it** — one factory + one operate callback (see `test_touptek_concurrency_stress.cpp`). Drivers without a registration are still covered only by code review against the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver); do not assume green CI means thread-safe for them. **SDK-callback paths especially**: the TSan suppressions mute any report with a vendor-blob frame on the stack, so a race in driver code invoked from an SDK internal thread is invisible to CI unless that callback path is exercised through a fake-SDK seam (fully instrumented, no suppression applies) — when you add an SDK callback to a driver, register a fake-seam stress path for it in the same change.

## Logging, Threading, and Errors

- Use AlpacaCore logging sink flow; do not use ad-hoc stdout/stderr logging in runtime paths.
- Avoid global mutable state; protect shared state with mutexes.
- Use `AlpacaException` for error paths; AlpacaHTTP maps exceptions to Alpaca error responses.
- **Disconnect paths must be exception-safe** (also in the concurrency checklist): in ref-counted SDK wrappers, erase the usage bookkeeping **before** the SDK close call so a throwing close (device unplugged) cannot leave a zero-count entry that turns later closes into no-ops; in driver `set_connected(false)`, clear driver state (`connected_`, handle/id, cached info) **before** the SDK close so a throw cannot trap the driver half-connected. The PlayerOne PW + ZWO EFW/EAF/CAA wrappers and drivers are the template.
- AlpacaHTTP must return Alpaca-style JSON envelopes and stable error mapping behavior.
- On-disk logging writes daily files `alpacabridge-YYYY-MM-DD.log` to `logging.directory` (default `/var/log/AlpacaBridge`, per-config override, env `ALPACAHTTP_LOG_DIRECTORY`). The sink falls back to `$XDG_STATE_HOME/AlpacaBridge/logs` (or `~/.local/state/AlpacaBridge/logs`) when the configured path is not writable. systemd unit uses `LogsDirectory=AlpacaBridge`; the deb postinst pre-creates the directory for non-systemd starts. There is no in-memory log buffer — `/management/v1/logs` reads today's daily file directly from disk.
- Retention: `logging.retention_days` (default 90, 0 = forever, env `ALPACAHTTP_LOG_RETENTION_DAYS`) auto-deletes daily files whose embedded date is older than `today − retention_days`. Pruning runs once on startup and again on day-rollover inside the file sink. Today's active file is never pruned.
- Web portal exposes `GET /management/v1/logfiles`, `GET /management/v1/logfiles/{name}[?download=1]`, and `DELETE /management/v1/logfiles/{name}`. Filenames are validated against the daily pattern to prevent path traversal. `util::read_log_file` enforces a 10 MiB per-request cap; web viewer warns and suggests download above 5 MiB.
- Log level set via `POST/PUT /management/v1/loglevel` is persisted to `config/runtime_state.json` and reapplied on the next start (overrides `default.yaml`'s `logging.level`). Delete that file to fall back to the YAML default. Persistence failures are logged at WARNING and never block the API response.
- Alpaca-style management responses (including the new logfile endpoints) return HTTP 200 even when `ErrorNumber != 0` — clients must inspect the body, not the HTTP status.

## Vendor-Specific Notes

Vendor notes contain **vendor specifics and deltas only** — general rules (concurrency,
ROI alignment, GPIO/soft-PWM, FilterWheel semantics, config round-trip) live in the
sections above. If a rule would apply to a second vendor, it belongs up there, not here.

### ZWO

Devices: Camera, FilterWheel, Focuser (EAF), Rotator, Switch (dew heater, ASIAIR Pro / Plus Pi CM4 / Plus RK3568 12V power), Telescope (AM mount).

SDK locations: `AlpacaCore/external/ZWO/ASI_Camera_SDK/`, `EAF/`, `EFW/`, `CAA/`, `AM/`. The ASIair Pro switch driver does not use an SDK — it talks directly to the on-board Pi 4 GPIO via libgpiod v2.

- ROI divisors: width%8, height%2 after binning (see [Camera ROI alignment](#camera-roi-alignment-all-camera-vendors)).
- Dew heater is exposed as an Alpaca Switch device (not a camera action) and is camera-dependent.
- ST4 pulse guiding should be enabled only when the SDK reports `has_st4_port`.
- PulseGuide: do not apply permanent RA/Dec offsets based on expected guide motion. If synthetic offsets are needed, keep them temporary and clear after the pulse completes to avoid double-counting mount motion.
- The ZWO and QHY SDKs both statically link libusb, causing duplicate symbol issues. The ZWO vendor `CMakeLists.txt` handles this — do not link both vendor static libs into the same binary without resolving the conflict.

#### ZWO ASIair Pro Switch (12V power ports via on-board GPIO)

End-user setup instructions live in [AlpacaCore/PowerPorts.md](AlpacaCore/PowerPorts.md); this section captures the implementation-side context.

- **Hardware reality**: ASIair Pro is a Raspberry Pi 4 (BCM2711) with a custom HAT exposing four 12V DC outputs. The stock ZWO firmware enables them at boot via `/boot/config.txt` under `[all]`: `gpio=18,12,13,26=op,dh,pu` (all four configured as output, default-high, pull-up). The boot-time `dh` flag is why all four DC ports come up powered as soon as the Pi boots — gear plugged in is "live" before any userspace runs.
- **Port-to-GPIO mapping** (Pi 4 ASIair Pro): Port 1 = GPIO 12, Port 2 = GPIO 13, Port 3 = GPIO 26, Port 4 = GPIO 18 on `/dev/gpiochip0`. Confirmed against the stock app via direct probe. Note: the order in `/boot/config.txt` (18,12,13,26) is *not* the port order.
- **Persistent state shape**: The stock ZWO `zwoair_imager` binary persists per-port settings in `~/.ZWO/ASIAIR_imager.xml` under the XPath `setting2/imager/gpio/port_N/` (zero-indexed: `port_0`..`port_3`). Each port has an `is_pwm` boolean flag. No per-port GPIO pin number is stored — the port-index→GPIO mapping is hard-coded in the stock binary. The AlpacaBridge driver makes the mapping configurable via `ports: [{gpio: N, pwm: bool}]` in the device config so it can be reused on other arm64 SBCs (e.g. RK3568-based ASIair Plus) with different wiring.
- **App role abstraction is cosmetic**: the ASIair mobile app lets users assign a *role* to each port (Mount / Camera / Focuser / Dew Heater / Flat Panel / Other). Roles "Dew Heater" and "Flat Panel" enable software PWM dimming; the others are plain on/off. Under the hood every port can do either — the "role" is just a UI tag that sets the `is_pwm` flag. The AlpacaBridge driver does not model roles; it exposes 4 ASCOM Switch channels and lets users name them however they want.
- **PWM delta**: the stock app uses pigpio's DMA-based soft-PWM at 40 kHz; this driver uses the standard userspace soft-PWM ([general rules](#gpio-power-switch--soft-pwm-drivers-general-rules)) at a 1 kHz default — dew heaters don't care about frequency, and it stays portable off-BCM.
- **libgpiod v2** per the general rule; for older Bullseye/Bookworm hosts install libgpiod 2.x from backports rather than dual-targeting.
- **OS architecture gate**: AlpacaBridge is arm64-only. The factory stock ASIair Pro ships **32-bit Raspbian Buster armv7l** — our `.deb` will not install on the stock OS. Deployment requires re-imaging with Raspberry Pi OS 64-bit (Bookworm or Trixie). Once re-imaged, the stock `zwoair_imager` / `pigpiod` daemons must be disabled because they hold the GPIO lines via pigpio and would prevent libgpiod from claiming them (EBUSY on `gpiod_chip_request_lines`).
- **Boot default-on / disconnect**: general boot-high-preserve and never-power-cycle rules; with the boot-time pull-ups a released line stays HIGH. Users wanting a different boot state must edit the `/boot/config.txt` `gpio=` directive — document in install notes for users migrating from stock.
- **Coexistence with stock app is not supported**: libgpiod and pigpio cannot share GPIO line ownership. The stock `pigpiod` daemon (started by `/etc/rc.local → /home/pi/ASIAIR/asiair.sh`) must be disabled, and the stock `zwoair_imager` must not run. There is no way to run AlpacaBridge alongside the stock ASIair app on the same device.

#### ZWO ASIAIR Plus Switch (Pi CM4 variant — same libgpiod path as the Pro)

The CM4-based ASIAIR Plus is electrically a Pi-class board and **reuses the existing libgpiod `asiair` driver unchanged** — there is no separate CM4 driver, wrapper, or test file. It is surfaced only as a router/UI alias. Confirmed against live hardware on 2026-05-31 (host `astro.lan` / `192.168.1.171` — earlier notes said `asiair.lan`; the box was renamed, same device — stock firmware).

- **Hardware reality**: Raspberry Pi Compute Module 4 (BCM2711). `/proc/device-tree/model` = "Raspberry Pi Compute Module 4 Rev 1.0"; `/dev/gpiochip0` = `pinctrl-bcm2711` (58 lines) — the same bank the Pro driver targets. The board also carries a PCA9685 at I²C `0x40` and an `asiair-overlay` referencing `pwm-2chan` / MCP23017 / AXP209, **but none of those drive the four DC power ports** — they are red herrings (LED/PMIC/expander). The DC ports are plain BCM GPIO.
- **Port→GPIO mapping is IDENTICAL to the Pi 4 ASIAIR Pro**: Port 1 = GPIO 12, Port 2 = GPIO 13, Port 3 = GPIO 26, Port 4 = GPIO 18 on `/dev/gpiochip0`, active-high, default-on at boot. The stock `/boot/firmware/config.txt` directive is `gpio=12,13,18,26,5,6,16,17=op,dh` (the four DC ports plus four extra control lines 5/6/16/17 that the v1 driver ignores). So `default_asiair_pro_config()` is correct as-is for the CM4 Plus.
- **Mapping verified by "drive a known config, read it back"**: stock-app duty cycles matched `pigs gdc` exactly (59% → 590, 34% → 340). Gotcha: a single `pigs r <pin>` snapshot of a PWM pin races the duty cycle and often reads 0 — use `pigs gdc` or sample ~200× before concluding anything about a pin.
- **Reuse wiring**: router accepts `switchType: "asiair-plus-picm4"` and routes it to `create_zwo_asiair_switch` / `default_asiair_pro_config()` with the same config sanitization as `asiair` (the `picm4` id was pre-reserved on the roadmap). Web UI adds an "ASIAIR Plus 12V Power Switch (Pi CM4)" dropdown option that reuses the Pro's per-port GPIO table. End-user setup lives in [AlpacaCore/PowerPorts.md](AlpacaCore/PowerPorts.md) under "ZWO ASIair Plus (Raspberry Pi CM4)".
- **Same OS / coexistence constraints as the Pro**: stock OS is 32-bit `armv7l` (kernel `5.10.27-v7l`) — requires re-imaging to arm64; the stock `pigpiod` / `zwoair_imager` must be disabled (libgpiod vs pigpio line-ownership conflict, `EBUSY`).
- **Model label (config-driven)**: the CM4 Plus reuses the Pro driver but reports the correct model. `AsiairSwitchConfig` carries a `model_name` (default `"ASIAIR Pro"`) that `get_name()`/`get_description()`/`get_driver_info()` interpolate; the router sets it to `"ASIAIR Plus (Pi CM4)"` for `switchType: asiair-plus-picm4`. So the same driver serves both the Pro and the CM4 Plus, differing only by this label. ConformU 4.3.0 re-validated 2026-06-04 on the live CM4 (host `astro.lan`, `192.168.1.171`) — 0 errors / 0 issues / 0 timing, reporting "ASIAIR Plus (Pi CM4)".
- **Branding**: user-facing strings were normalized "ASIair" → **"ASIAIR"** (ZWO's actual product branding) across both switch drivers, the Web UI, and tests. Internal `switchType` ids (`asiair`, `asiair-plus-picm4`, `asiair-plus-rk3568`), log categories, and the gpiod consumer label stay lowercase/unchanged.

#### ZWO ASIair Plus Switch (RK3568 variant — kernel module ioctl, not libgpiod)

- **Hardware reality**: ASIair Plus (RK3568) is a Rockchip RK3568 SoC with a custom ZWO HAT. The four 12V DC outputs are **not** standard Linux GPIO from the user-space side — ZWO ships a custom kernel module (`pwm_gpio.ko`, author "JerryCui") that owns the device-tree `airplus-gpios` node and registers a misc-device character node at `/dev/pwm-gpio-misc` (major 10, minor 55). All port control goes through documented ioctls on that fd. This means standard `libgpiod` / `gpioget` tools see the lines as already-claimed and cannot drive them.
- **Reverse-engineered ioctl header**: vendored at `AlpacaCore/external/ZWO/asiair-plus/pwm_gpio.h`. Source: decoded from DWARF debug info on the production ZWO kernel module. Defines `gpio_level_t`, `pwm_param_t`, `work_mode_t` plus the ioctl numbers (`PWM_GPIO_SET_LEVEL`, `PWM_GPIO_SET_MODE`, `PWM_GPIO_SET_CONFIG`, etc.). Treat this header as the canonical interface contract — do **not** invent new ioctls without confirming against the module disassembly.
- **Port index mapping (kernel module side)**: indices 0..11. Indices 0, 1 = LEDs; 2 = physical button (input); **3 = "DC master enable / control signal"** (must be HIGH for the DC ports to deliver voltage); **4..7 = the four DC power ports (PWM-capable)**; 8..11 = USB2/USB3 power enables. The AlpacaBridge driver exposes only indices 4..7 as wrapper indices 0..3 — USB power and button are deliberately out of scope for the v1 Switch driver to keep the ASCOM contract focused. Document any future extension here.
- **open() is read-only — do NOT touch any kernel-side state on connect.** Two earlier attempts to "initialize" the kernel module from open() — first driving the master-enable line at kernel index 3, then explicitly setting mode + level on each DC port — both produced the same symptom: every DC port physically dropped to 0 V the moment an ASCOM client connected. The kernel module is closed-source / reverse-engineered. `open()` opens the fd and does nothing else. The constructor's optimistic cache (value=1 / 100 = "on") is left in place — matches typical boot-time behavior of all four DC ports being live. The first hardware write happens only when the ASCOM client calls `SetSwitch` or `SetSwitchValue`. Do not add any init-time write here.

- **PWM_GPIO_GET_LEVEL lies.** Empirically (probed across all 12 kernel indices on a freshly-booted device with all DC ports physically powered and the network LED lit): `GET_LEVEL` returns 0 for every index. It reads the kernel module's "last-written via SET_LEVEL" cache, which initializes to 0 at module load regardless of what pinctrl drove the pads to physically. So **never** trust `GET_LEVEL` for state introspection — the UI will silently flip to "all off" on first connect after fresh boot, even though every port is hot. Read `/sys/kernel/debug/gpio` (line directions and levels) for true state when debugging.

- **The kernel module needs SET_MODE → ENABLE → SET_LEVEL.** Reverse-engineered the hard way after observing that toggles in NINA had zero physical effect even though every ioctl returned 0. The kernel debug interface revealed the DC port GPIO lines (GPIO 146/147/149/150 on gpiochip4) were stuck as **inputs** with external pull-ups holding them HIGH, regardless of how many `SET_LEVEL` calls our driver made. The kernel module silently rejects writes to lines that aren't in OUTPUT direction (it logs `"NOT in GPIO OUTPUT mode"` via printk but returns 0 to userspace). The fix: call `PWM_GPIO_ENABLE` between `SET_MODE` and `SET_LEVEL` — internally that maps to `gpiod_direction_output_raw` and flips the line.

- **SET_LEVEL polarity is INVERTED from typical gpiod semantics.** Verified by reading `/sys/kernel/debug/gpio` after each ioctl AND physically observing a 12V flat panel on DC port 2: `SET_LEVEL(0)` resolves the line to `in hi` (input, pulled high externally) which **powers the panel ON**; `SET_LEVEL(1)` resolves to `out lo` (output driven low) which **cuts power**. So in the wrapper, ASCOM `value=1` (on) maps to ioctl `level=0`, and `value=0` (off) maps to ioctl `level=1`. This is non-standard — typical gpiod chips treat `level=1` as drive-high — and the cause is almost certainly a quirk in the closed pwm_gpio.ko's level-argument interpretation (likely `level=1` maps internally to `gpiod_direction_output_raw(0)` while `level=0` maps to `gpiod_direction_input`, judging by the kernel debug state transitions). Don't "fix" this by flipping it back — the previous polarity produced the inverted-feeling NINA behavior the user reported on 2026-05-30 ("when I flip it on it goes off") and connected gear lost power on every value-set transition.

- **The kernel module's PWM mode is unreachable from documented ioctls — we do userspace soft-PWM instead.** SET_MODE(PWM) → ENABLE → SET_CONFIG returns success on every input (GET_CONFIG echoes back the period/duty we wrote), but the kernel's hrtimer dispatch branch never actually arms — `/proc/timer_list` shows no `pwm_gpio_timer_func` scheduled regardless of ordering, period, or duty. Disassembly of `pwm_gpio_misc_ioctl` shows the `hrtimer_init` + `hrtimer_start_range_ns` call sites do exist at file offset 0x7c0–0x7ec inside the misc-ioctl dispatcher, but they're gated by a condition we cannot trigger from userspace. The closed source means we can't confirm the gate definitively (the inventory's "GPL" license claim notwithstanding — try ZWO support if source-level fixes are ever needed). The DTS also confirms there's no hardware PWM controller mapped to the airplus-gpios pins on GPIO bank 4, so even if we triggered the hrtimer branch we wouldn't get hardware PWM — just kernel-side soft-PWM. So we run the standard userspace soft-PWM ([general rules](#gpio-power-switch--soft-pwm-drivers-general-rules)) over `SET_LEVEL`, with this module's inverted polarity. Confirmed against the extracted stock `zwoair_imager` daemon (`pwm_gpio_start` symbols — same approach). Default 50 Hz = the stock daemon's actual `period_ns = 20,000,000`, read back live via `PWM_GPIO_GET_CONFIG` (see the comment in `default_asiair_plus_rk3568_config`); range 1–100,000 Hz via `pwmFrequencyHz`.

- **Module forensics one-liners**: license GPL, author `JerryCui` — source must be obtainable from ZWO if kernel-side fixes are ever needed. Runtime introspection: `cat /sys/kernel/debug/gpio` (true line directions/levels), `/sys/firmware/devicetree/base/pinctrl/airplus_gpios/airplus-ports` (pin map).
- **Disconnect**: general never-power-cycle rule — the kernel module retains per-port mode + level across opens, so releasing our fd power-cycles nothing.
- **Permissions**: `/dev/pwm-gpio-misc` is created with root-only mode by the kernel module. We ship a udev rule (`AlpacaCore/external/ZWO/asiair-plus/99-zwo-asiair-plus.rules`) that grants the `gpio` group `0660` access. `build_and_run.sh` and the `.deb` postinst both install it via the existing rules-discovery loop in `external/`. The AlpacaBridge daemon user (and any human user wanting to poke at the device) must be in the `gpio` group.
- **Kernel module hard dependency**: `pwm_gpio.ko` ships only with ZWO's stock kernel build (4.19.219). Re-flashes that swap to mainline RK3568 distros (Armbian, etc.) will **not** include it, and the driver will fail to open `/dev/pwm-gpio-misc`. The flashing tool tracked at `rk-flashtool` is the supported path; document any alternatives here as they emerge.
- **Router config schema** (`switchType: "asiair-plus-rk3568"`): much simpler than the Pro because the kernel module fixes the index mapping. Fields are `devicePath` (default `/dev/pwm-gpio-misc`), `pwmFrequencyHz`, and a `ports[]` array where each entry is just `{ name, pwm }`. No `gpio` / `gpioChip` fields — they would be meaningless for this hardware.
- **Web UI naming convention**: device label is "ASIair Plus 12V Power Switch (RK3568)" — explicit because a Pi-CM4 variant (`asiair-plus-picm4`) is on the roadmap. Do not collapse the two to a bare `asiair-plus` `switchType`; the kernel-interface difference between the CM4 (libgpiod, like the Pro) and the RK3568 (kernel module, like this driver) is fundamental.

### QHY

Devices: Camera, FilterWheel (integrated CFW on cameras like the miniCam8M).

SDK location: `AlpacaCore/external/QHY/sdk_linux_arm64_26.06.04/`.

- Camera IDs are strings (`char[32]`), not integers — use `std::optional<std::string>` for camera_id and `std::optional<int>` for camera_index.
- `GetQHYCCDSingleFrame()` blocks until the frame is ready; run it in a background thread and use an exposure status enum (Idle/Working/Success/Failed) to communicate results.
- Temperature control requires `ControlQHYCCDTemp()` to be called approximately every second; use a dedicated background thread started/stopped with the cooler.
- `ControlQHYCCDGuide()` blocks the calling thread for the full pulse duration (confirmed on real miniCam8M hardware: a 2000ms pulse blocked the caller for exactly 2000ms) — run it on a detached thread (only the shared_ptr guiding flag + a copied camera ID, never `this`) so `PulseGuide` returns immediately, matching ASCOM's async expectation.
- `ControlQHYCCDTemp()` and `SetQHYCCDParam()` (at least for `MANULPWM`) have no SDK-side timeout and can occasionally run far past `ControlQHYCCDTemp`'s documented ~10s PID-loop figure. See the "blocking SDK call with no timeout" rule in Driver concurrency & lifecycle above — the temp-control and cooler-off worker joins in `qhy_camera_driver.cpp` are bounded (2s) and detach on timeout, and `qhy_sdk_wrapper.cpp`'s handle is reference-counted (`shared_ptr<qhyccd_handle>`) so that detach can never race a concurrent `close_camera()`.
- Guide direction convention differs from Alpaca: QHY uses EAST=0, NORTH=1, SOUTH=2, WEST=3 vs Alpaca North=0, South=1, East=2, West=3 — map explicitly.
- After changing readout mode, refresh chip info and reset ROI — sensor dimensions can change per mode.
- SDK global lifecycle (`InitQHYCCDResource` / `ReleaseQHYCCDResource`) is managed as a singleton in the wrapper; include `#define __CPP_MODE__ 1` before `#include <qhyccd.h>` in the wrapper `.cpp` only.
- Cameras require firmware files (`/lib/firmware/qhy/*.img` / `*.HEX`) in addition to udev rules. The udev rules call `fxload` to load firmware on plug-in, after which the device re-enumerates with a different USB product ID. Install firmware from `AlpacaCore/external/QHY/sdk_linux_arm64_26.06.04/lib/firmware/qhy/` to `/lib/firmware/qhy/`.
- The system `fxload` from apt does **not** support `-t fx3` (FX3-based cameras) and will exit 255 silently — always install the QHY SDK's own `fxload` binary from `sdk_linux_arm64_26.06.04/sbin/fxload` to `/sbin/fxload` instead.
- Re-enumeration in VMs: after `fxload` fires, the camera disconnects as `1618:c268` (Cypress WestBridge) and reconnects with its operational product ID. VMware and similar hypervisors will not automatically pass through the re-enumerated device unless the USB filter covers the entire QHYCCD vendor ID (`1618`). Test QHY cameras on bare metal or RPi rather than VMs where possible.
- **Integrated CFW (filter wheel) shares the camera's physical handle**: the miniCam8M and similar models have a color filter wheel accessed through the SAME `qhyccd_handle` as the camera — there is no separate CFW enumeration or `Open`/`Close`. `qhy_filterwheel_driver.cpp` is a second `AlpacaDriver` (device type `FilterWheel`) that resolves the SAME `cameraId`/`cameraIndex` as the paired camera device and calls the same `QHYSDKWrapper::open_camera()`/`close_camera()`. This required making the wrapper's handle map reference-counted (`Impl::SharedHandle{handle, open_count}`, keyed by camera_id): the first opener's `OpenQHYCCD` stays live and shared while either the camera driver or the CFW driver is connected, and only the last owner's `close_camera()` actually erases the entry and lets the `shared_ptr` deleter run `CloseQHYCCD` — mirrors ToupTek's `open_shared_by_id`/`close_shared` for its camera+thermal-switch pairing. **Any future QHY accessory that shares a camera's handle must go through `open_camera`/`close_camera`, never a raw `OpenQHYCCD`/`CloseQHYCCD`,** or it will silently steal/close the other owner's handle.
- **CFW does not need `InitQHYCCD`**: the 25.09.29 SDK's `testapp/common/ControlCFW.cpp` sample opened the camera and called `SendOrder2QHYCCDCFW`/`GetQHYCCDCFWStatus` directly with no `InitQHYCCD` in between, and this was validated on real miniCam8M hardware (ConformU clean run). The filter wheel driver therefore only calls `open_camera()`, not `init_camera()`, so it can connect and move the wheel even if the camera driver (which does call `init_camera()`) is never connected in the same session. Note the 26.06.04 SDK dropped that sample; its surviving `testapp/cmake_demo/test_cfw` demo uses a different style (`InitQHYCCD` + `Set/GetQHYCCDParam(CONTROL_CFWPORT)`), so re-validate the no-init SendOrder/Status path on hardware when bumping SDKs.
- **CFW protocol is a single ASCII digit, not a raw byte**: `SendOrder2QHYCCDCFW(handle, &order, 1)` expects `order = '0' + position` (e.g. slot 3 → the character `'3'`), and `GetQHYCCDCFWStatus(handle, status)` reports the settled position the same way — `status[0]` is `'0'`..`'9'` when the wheel has arrived, and any other value (commonly non-digit) while it's still moving. That "still moving" case maps directly onto the ASCOM FilterWheel `Position` "-1 while moving" sentinel, so `QHYSDKWrapper::get_cfw_position()` returns `-1` for it and the driver passes the value straight through with no separate is-moving flag (same shape as ToupTek AFW's `FILTERWHEEL_POSITION`).
- **Slot count is hardware-reported, not user-fixed**: `GetQHYCCDParam(handle, CONTROL_CFWSLOTSNUM)` (control ID 44) returns the wheel's actual slot count at connect; `IsQHYCCDControlAvailable(handle, CONTROL_CFWPORT)` (control ID 17) detects whether a CFW is present at all — connecting the filter wheel device against a QHY camera with no CFW throws `NotConnected` rather than silently reporting a fake 0-slot wheel. The web UI's slot-count picker (5/7/8/9/Custom, matching QHY's CFW slot-count lineup) is config-only, purely to pre-seed filter names before the first connect — same convention as ZWO EFW/ToupTek AFW.
- **`GetQHYCCDCFWStatus` has no "moving" sentinel — it reports the wheel's ACTUAL passing position throughout the physical rotation** (ConformU finding on real miniCam8M hardware): unlike ToupTek's `FILTERWHEEL_POSITION` (which returns `-1` for the entire in-motion window), this SDK call returns whatever slot the wheel is currently near while it physically rotates through intermediate slots on the way to the target (observed sequence for a 4→3 move: `4→5→6→-1→0→1→2→3`, i.e. it went the "short way" round through 5/6/0/1/2, and only returned `-1` briefly at one ambiguous point) and only settles on the commanded value once truly arrived. Each raw call is also a genuine ~100-130ms hardware round trip, which blows ConformU's FAST (0.1s) target for the first `Position`/`DeviceState` read after `Connect`. **Do not cache the first post-move reading unconditionally** — an earlier version of this driver did exactly that (cache on any non-negative digit) and it froze `Position` at the stale pre-move slot forever, failing every ConformU move test with a 30s timeout, because literally every raw reading looks like a valid "settled" digit including the ones taken mid-rotation. The fix: (1) one warm-up read during `Connect` (charged against the STANDARD 1.0s budget) seeds a settled-position cache so the two connect-adjacent FAST-classified reads (`DeviceState`, the first `Position` Get) are served from cache; (2) `set_position()` records the commanded target and clears the cache; (3) while a target is pending, `get_position()` always does a live read, and if that read doesn't match the pending target it is **masked to `-1`** rather than passed through raw — a real client (NINA) polling mid-move otherwise sees the wheel's actual but unrelated transit slot (e.g. `5` during a `4→3` move) and reports it as a mismatched/erroneous arrival before the wheel has actually settled. Only once a live read equals the pending target do we cache it and resume serving from cache. See `qhy_filterwheel_driver.cpp`'s `get_position()`/`set_position()` for the implementation.

### SVBONY

Devices: Camera.

SDK location: `AlpacaCore/external/SVBONY/lib/armv8/`, headers under `external/SVBONY/include/`.

- **Control warm-up at connect (SV905C2 quirk)**: After `SVBOpenCamera`, `SVBSetControlValue(SVB_GAIN, ...)` returns `SVB_ERROR_GENERAL_ERROR` indefinitely on SV905C2 — regardless of value, regardless of `bAuto` flag, regardless of whether `SVBStartVideoCapture` is active, and `SVBRestoreDefaultParam` does not clear the state. The driver works around this by iterating every writable control reported by `SVBGetControlCaps` and writing each to its `default_value` during the connect path (after `SVBSetROIFormat` / `SVBSetOutputImageType`). Once any `SVBSetControlValue` call has landed, subsequent client gain writes succeed. Failures during the warm-up are tolerated and logged at DEBUG. Do not remove the warm-up loop in `set_connected` without re-running ConformU against an SV905C2 — the failure is silent until a client tries to set gain. Likely related to SDK readme entries `v1.13.1: Fixup ASCOM software to support SV905C2` and `v1.13.2: Optimize gain settings of SV905C2`.
- **Auto control writes**: `disable_auto_if_needed` reads the current value/auto flag and only writes back if currently auto, since some SVBONY models reject manual writes while auto is active with the same `SVB_ERROR_GENERAL_ERROR`.
- **`SVBSetControlValue` retry**: The wrapper retries up to 3 times with a 50 ms backoff specifically on `SVB_ERROR_GENERAL_ERROR` to absorb genuinely transient hardware-op faults; deterministic rejections still surface after the retries are exhausted.
- **Camera mode**: We use `SVB_MODE_NORMAL` (continuous video) and start/stop `SVBStartVideoCapture` per exposure. INDI's `indi-svbony` driver instead uses `SVB_MODE_TRIG_SOFT` with persistent video capture for stills — keep this in mind if a future SVBONY model needs trigger-mode behavior.
- **Bin/ROI quirks**: divisors width%8, height%2 (see [Camera ROI alignment](#camera-roi-alignment-all-camera-vendors)). ROI updates and `FrameSpeedMode` writes are deferred to `start_exposure` because some SDK control writes take ~1.1 s and would otherwise blow ASCOM client timing budgets.
- **`SVBRestoreDefaultParam`** is called immediately after `SVBOpenCamera` to clear any leftover state from a previous session, mirroring `indi-svbony`. Tolerate failure for older SDK builds that don't export the symbol.

### ToupTek

Devices: Camera, Focuser (AAF — Astro Auto Focuser), FilterWheel (AFW — Astro Filter Wheel, AFW-M 5/7-slot), Switch (two backends: cooled-camera **Thermal** — dew heater + fan; and the **StellaVita PowerBox** — GPIO).

SDK location: `AlpacaCore/external/ToupTek/toupcamsdk.20260128/` (shared between the camera, focuser, filter-wheel, and thermal-switch drivers). The StellaVita Switch driver uses **no SDK** — it is a libgpiod-only driver that happens to live under the ToupTek vendor.

- **Single SDK, multiple device types**: camera, focuser, filter-wheel, and thermal-switch drivers all go through `ToupTekSDKWrapper`. Cameras enumerate via `enumerate_cameras()`, focusers via `enumerate_focusers()` (filters `Toupcam_EnumV2` by `TOUPCAM_FLAG_AUTOFOCUSER`), filter wheels via `enumerate_filter_wheels()` (filters by `TOUPCAM_FLAG_FILTERWHEEL`). Same `Toupcam_Open` selects a camera/focuser/wheel by its capability flag. **`enumerate_cameras()` must EXCLUDE the accessory flags** (`TOUPCAM_FLAG_FILTERWHEEL | TOUPCAM_FLAG_AUTOFOCUSER`) — `Toupcam_EnumV2` returns AFW wheels and AAF focusers in the same list, and the camera driver resolves `cameraIndex` as a *position into the enumerate_cameras() vector* (then opens by that entry's id), so an unfiltered list makes `cameraIndex=0` silently open the filter wheel when both are attached. The three enumerations partition the devices: cameras = neither accessory flag.
- **Runtime sensor-register writes** (ToupTek mechanics of the exposure-guard rule in the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver)): the ToupTek camera is the *only* driver that programs sensor registers at runtime — CG/HFW (`put_cg`/`put_high_fullwell`), gain (`put_gain`), black level (`put_blacklevel`); the ZWO/SVBONY/Player One `set_readout_mode` are no-op stubs. `SetReadoutMode`/`Gain`/`Offset` do the `mutex_`-taking validation first (`handle_copy()`, ranges), then `lock(readout_mutex_)` → `ensure_connected()` → `ensure_not_exposing()` → SDK write. The geometry setters (`set_bin`/`set_num_x/y`/`set_start_x/y`) also take `readout_mutex_` + `ensure_not_exposing()` — not for the SDK (they only set `*_dirty_` flags) but because mutating `bin_`/`num_x_`/`start_*_` mid-integration desyncs the in-flight frame's geometry from the next buffer size.
- **Abort mechanics** (ToupTek mechanics of "wake the SDK wait before joining"): the exposure thread parks in `Toupcam_WaitImageV4` (the wrapper deliberately does NOT hold the SDK lock across it). `stop_exposure`/`stop_exposure_thread` call `sdk.stop(handle_)` **under `mutex_`** to unblock the wait before joining; that halts the pull-mode stream (started once at connect via `start_pull_mode`), so they set `format_dirty_`+`roi_dirty_` to force the next `start_exposure` to re-init it, and `stop_exposure_thread` runs *before* the ROI snapshot so that re-init lands in the same exposure. Don't pre-clear `exposure_active_` before the join — let the worker clear it via the stopped stream, else a concurrent register write sees a false "idle" mid-frame.
- **ROI dirty-flag timing** (ToupTek mechanics of "clear flags after validation"): `start_exposure` snapshots `format_dirty_`/`roi_dirty_`, validates the ROI, then clears them at the *end* of the locked block; the worker's catch re-marks *only* the stage (`format`/`roi`) that didn't complete (via local `*_applied` bools) so a failed apply doesn't force a needless stream restart.
- **Two device drivers can share ONE camera (reference-counted open)**: `Toupcam_Open` allows only one handle per physical camera, but the thermal switch (dew heater/fan) must operate the *same* camera the Camera device is streaming from. `ToupTekSDKWrapper` reference-counts opens by the device's opaque id (`shared_by_id_` / `id_by_handle_` maps) via the shared `Impl::open_shared_by_id` / `close_shared` helpers: `Toupcam_Open` fires only for the first opener and returns the shared `HToupcam`; `Toupcam_Close` fires only when the last holder releases. **ALL open-by-id paths route through these helpers — camera, focuser, AND filter wheel** (`open_camera_by_id`, `open_focuser_by_id`, `open_filter_wheel_by_id` all delegate); don't reintroduce a raw `Toupcam_Open`/`Close` in any of them. Distinct physical devices enumerate to distinct ids so they never collide, but two driver instances on the *same* id (the camera + its thermal switch, or a camera + its integrated autofocuser) now correctly share one open instead of the second raw-open returning null. Mirrors the Player One wrapper's `usage_`/`open_count`. Consequence: connecting the camera *and* the thermal switch is one physical open; disconnecting the camera while the switch is still connected keeps the camera powered/cooling (desirable). Opens by *index* (`open_camera_by_index`) are not tracked and close immediately (legacy path).
- **Offset = black level**: ASCOM `Offset` maps to `TOUPCAM_OPTION_BLACKLEVEL`, gated on `TOUPCAM_FLAG_BLACKLEVEL`. Integer `OffsetMin`(0)/`OffsetMax` mode (no named `Offsets` list). `OffsetMax` scales with the current output bit depth — `31 << (bits - 8)` (`TOUPCAM_BLACKLEVEL8_MAX` = 31), where bits is 8 in 8-bit output mode else the camera's deep bit count — so it's computed in the wrapper (`get_blacklevel_max`) which reads `OPTION_BITDEPTH`. ToupTek was previously the only camera driver stubbing offset to `PropertyNotImplemented`; it now matches ZWO/SVBONY/Player One/QHY. **`FullWellCapacity` is NOT queryable from the SDK** — the driver returns the ADU saturation (`2^bitdepth − 1`), not electrons; the true full well is a sensor datasheet spec (IMX571: ~51 ke⁻ Normal, ~100 ke⁻ High Full Well), and the High Full Well ReadoutMode is what switches between them.
- **Conversion gain + High Full Well → ReadoutModes**: the two ToupTek sensor-mode axes — conversion gain (`TOUPCAM_OPTION_CG`: 0=LCG, 1=HCG, 2=HDR-if-`FLAG_CGHDR`) and High Full Well (`TOUPCAM_OPTION_HIGH_FULLWELL`) — are folded into ONE flat ASCOM `ReadoutModes` list (ASCOM has only one readout-mode axis). `readout_mode_specs()` builds the list from capabilities: `HCG/LCG(/HDR)/High Full Well` when both, `HCG/LCG(/HDR)` for CG-only, `Normal/High Full Well` for HFW-only, else `Normal`. **Each spec fully specifies BOTH axes** (e.g. "High Full Well" = CG-LCG + HFW-on) so `get_readout_mode` round-trips to a stable index by reading both options. This is the idiomatic ASCOM home for hardware sensor modes (NINA shows a dropdown), NOT a custom Action. Both `get_readout_modes()` (the list) and `get_readout_mode()` (the current index) throw `NotConnected` while disconnected (ASCOM contract — keep them consistent), while `set_readout_mode` validates the range *before* the connection check so an out-of-range index is `InvalidValue` even disconnected. `preload_camera_info()` populates caps at *construction*, so a unit test must NOT assert a specific mode list (it depends on the attached camera); assert only the hardware-independent invariants — both getters throw `NotConnected` disconnected, and out-of-range `set_readout_mode` throws `InvalidValue`.
- **Odd bin factors need an even ROI span (3×3 hang)**: `Toupcam_put_Roi` coordinates are in ORIGINAL (sensor) resolution for digital binning (SDK header note (a)), and the SDK requires **even** width/height/offset. The binned ROI span is `num × bin`; for an odd bin factor that product can be odd (full-frame 3×3 on the ATR2600M → 4167-tall), which `put_Roi` rejects — the exposure then hangs and `ImageReady` never sets (2×2/4×4 are always even, so only 3×3 fails ConformU). Fix in `start_exposure`: round the sensor span UP to even and the offset DOWN to even; digital binning floor-bins the padded span back to exactly `num` pixels (the +1 pad is `< bin` for any `bin ≥ 2`), so the buffer and reported `NumX`/`NumY` stay correct. **Crucially, derive the max binned dimension from the EVEN sensor size** (`(max_width & ~1) / bin`, not `max_width / bin`): the largest deliverable binned width is `floor(even_max/bin)`, so a client requesting `floor(raw/bin)` on an odd-width sensor can't ask for a span that, once even-rounded, exceeds the sensor and clamps back to fewer than `num` columns (a zero-filled black edge column). With the limit derived from the even size, `ceil_even(num×bin) ≤ even_max` always holds and the clamp is unreachable. Keep a small buffer margin as crash-insurance.
- **Thermal switch (dew heater + fan + tail LED)** — `touptek_thermal_switch_driver.{h,cpp}`, mirroring `playerone_switch_driver`. Dew heater = `TOUPCAM_OPTION_HEAT` (level 0..`OPTION_HEAT_MAX`), fan = `TOUPCAM_OPTION_FAN` (speed 0..`model->maxfanspeed` — the fan max comes from the enumerated model, not an option), tail indicator LED = `TOUPCAM_OPTION_TAILLIGHT` (boolean on/off; astro users turn it off to avoid reflections/light leaks). Heater/fan are capability-probed via `TOUPCAM_FLAG_HEAT`/`_FAN`; the **tail LED has no capability flag**, so it is probed by *reading* `get_taillight` in a try/catch and only exposed if the camera accepts it. A camera exposing none of the three throws `NotImplemented`. `kMaxThermalElements` = 3 (the disconnected switch-ID bound). **The cooler is NOT a switch element** — it lives on the Camera interface (`CoolerOn`/`SetCCDTemperature`/`CoolerPower`), matching ASCOM and Player One. The `(touptek, switch)` router branch and the config sanitizer pick the backend from `switchType`: `"thermal"` (bound by `cameraIndex`) vs `"stellavita"` (default, GPIO). The thermal switch builds on any ToupTek host (camera SDK only); StellaVita still needs libgpiod. **Gotcha (bit us):** the web-UI switch-type `<select>` must use `name="touptekSwitchType"`, not the bare `switchType` — ZWO's hidden select wins the FormData collision and the ToupTek switch silently registers as StellaVita (fails to connect: no GPIO). The discriminator-select instance of the [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors) rule.
- **AFW (Astro Filter Wheel) — option-based, no dedicated API**: unlike the AAF focuser (which has the `Toupcam_AAF` action interface), the filter wheel is driven through the generic `Toupcam_get/put_Option`. Slot count = `TOUPCAM_OPTION_FILTERWHEEL_SLOT` (read once at connect, like ZWO EFW's `slotNum` — never hardcode 5/7; the web UI's 5/7/Custom picker is config-only). Position = `TOUPCAM_OPTION_FILTERWHEEL_POSITION`: **get returns `-1` while in motion** — that maps *directly* onto the ASCOM FilterWheel `Position` "moving" sentinel, so pass it through unchanged (no extra is-moving flag needed). On set, the low byte is the target slot (mask with `& 0xff`) and `(val>>8)&0x1` is a direction bit (`0` = clockwise, `1` = "auto direction"). **The wheel MUST be homed at connect or it hunts and never lands** — the single most important thing about this driver, and it bit us hard: without homing, moves make the wheel tick/rock in place ("tick-tick pause, never completes a revolution"), especially right after a firmware update (the firmware loses its slot reference). The fix mirrors the **INDI `indi_toupwheel` reference driver's** connect sequence exactly: (1) `get_Option(FILTERWHEEL_SLOT)` read the slot count; (2) `put_Option(FILTERWHEEL_SLOT, slot)` write it straight back (re-applies the wheel's slot config; the option is `[RW]`); (3) `put_Option(FILTERWHEEL_POSITION, -1)` home/reset so the firmware references its slots (in INDI this is `SelectFilter(0)` → `put_Option(POSITION, SpinningDirection | (0-1))` = `-1`). `reset_filter_wheel` returns immediately but the firmware takes ~1.5 s to home — **wait for the home to settle (poll `get_filter_wheel_position` until it returns a non-negative slot; it reports `-1` while moving) BEFORE reporting `Connected`**, otherwise a `SetPosition` arriving during the home window aborts the cycle and leaves the slot reference unknown (moves then land on wrong slots — the exact failure homing was added to prevent). Once homed, a **single absolute move** (`put_Option(POSITION, position)`, 0-based, direction bit `0`) traverses to any slot fine — do NOT step one slot at a time, and do NOT set the direction bit to `1`/auto (that made single-slot moves oscillate in place on our hardware). ASCOM `Position` is already 0-based so no `±1` offset is needed (INDI carries a `-1`/`+1` only because it is internally 1-based). The wrapper hides these option constants inside `touptek_sdk_wrapper.cpp` (same as the camera `TOUPCAM_OPTION_*` calls) so driver code never includes `toupcam.h`.
- **AFW driver mirrors ZWO EFW for filter semantics, ToupTek focuser for handle/connection**: `Names`/`FocusOffsets` normalization (length tied to slot count, single-string-to-chars expansion, default `Filter N` names) is copied verbatim from `zwo_filterwheel_driver.cpp`; the `HToupcam handle_` + async connection-thread lifecycle is copied from `touptek_focuser_driver.cpp`. Constructed by index or SDK id (string), same as the ToupTek focuser — not the ZWO `int wheel_id`.
- **AAF API convention** (`Toupcam_AAF(handle, action, value, *out)`):
  - **SET**: `Toupcam_AAF(h, AAF_SETxxx, value, nullptr)` — passes value in third arg.
  - **GET**: `Toupcam_AAF(h, AAF_GETxxx, 0, &out)` — third arg is unused, output via pointer.
  - **RANGE-of-GET**: `Toupcam_AAF(h, AAF_RANGEMAX, AAF_GETxxx, &out)` — queries the upper bound of a GET property by passing the GET action code as the third arg. Used to discover MaxStep and Backlash range at connect time. Same pattern works for `RANGEMIN` and `RANGEDEF`.
  - **HALT/SETZERO**: control actions, third arg is the new value (0 for halt; ticks for setzero/sync).
- **`AAF_GETSTEPSIZE` is mechanically meaningful only for specific focuser configurations** — the driver does not expose it as ASCOM `StepSize`. It throws `PropertyNotImplemented` per the focuser ASCOM contract.
- **Temperature units**: `AAF_GETTEMP` returns tenths of Celsius (e.g. `32` → `3.2 °C`). Divide by 10.0 before returning to ASCOM. INDI applies a 0.1 °C hysteresis when updating UI; we read on demand so the hysteresis is unnecessary on the driver side.
- **No temp-comp action**: The AAF action set has no temp-comp control. `TempCompAvailable` returns false; `set_temp_comp(true)` throws `NotImplemented` (not `DriverException`).
- **`Toupcam_get_FocusMotor` is deprecated** in the shipped SDK header. Do not use it for AAF focusers — use the `Toupcam_AAF` action interface instead. The non-deprecated `FocusMotor` API is for autofocus-equipped cameras (`TOUPCAM_FLAG_FOCUSMOTOR`), a different capability.

#### StellaVita PowerBox (Switch)

The StellaVita is a **Raspberry Pi CM4 (BCM2711)** observatory controller. Its four on-board 12V DC ports are local GPIO outputs driven via **libgpiod v2** — there is no SDK and no camera-SDK dependency. Files: `touptek_powerbox_wrapper.{h,cpp}` (libgpiod backend, same design as `ioptron_powerbox_wrapper`) and `touptek_switch_driver.{h,cpp}`. Built only when libgpiod (>= 2.0) is present; the `ALPACACORE_TOUPTEK_STELLAVITA` CMake cache var gates the AlpacaHTTP router branch and the unit test to match (same pattern as `ALPACACORE_IOPTRON_POWERBOX`).

- **GPIO mapping (verified on hardware)**: the four DC ports are BCM GPIO **18 (Port 1), 10 (Port 2), 17 (Port 3), 4 (Port 4)** on `/dev/gpiochip0` (`pinctrl-bcm2711`), where the libgpiod line offset equals the BCM GPIO number. Source of truth is the board's `config.txt`: `gpio=18,10,17,4,9,11=op,dh,pu`.
- **GPIO 9 and 11 are deliberately excluded**: that same config.txt line drives them high too, but they power the on-board **Cypress USB hub** — exposing them as switch channels would let a client cut power to every attached USB camera/focuser. Never add them to `default_stellavita_config()`.
- **Boot-high preserve / never-power-cycle**: general rules (`op,dh,pu` boot directive).
- **PWM frequency 100 Hz** (`default_stellavita_config()`): hardware-tested sweet spot — some panels flicker at the 50 Hz iMate/ASIAIR default.
- All four ports are writable and boolean by default, each switchable to soft-PWM (0–100%) — unlike the iMate there is no always-on read-only pass-through port.

### Player One

Devices: Camera, FilterWheel (Phoenix Wheel), Switch (thermal: dew heater + fan on cooled cameras).

SDK locations: `AlpacaCore/external/PlayerOne/PlayerOne_Camera_SDK_Linux_V3.10.0/` (cameras) and `AlpacaCore/external/PlayerOne/PlayerOne_FilterWheel_SDK_Linux_V1.2.3/` (Phoenix Wheel). These are **two unrelated SDK libraries** (`libPlayerOneCamera`, `libPlayerOnePW`) with separate C APIs — the filter wheel has its own wrapper (`playerone_pw_wrapper`, mirroring `zwo_efw_wrapper`) rather than extending `playerone_sdk_wrapper`. Both `.so` files ship in the `.deb` and via the install scripts.

- **Guide direction mapping (camera)**: Player One ST4 guide config IDs map North=0/South=1/East=2/West=3 — already matching ASCOM order.
- **ROI alignment (camera)**: divisors width%4, height%2 (see [Camera ROI alignment](#camera-roi-alignment-all-camera-vendors)).
- **Wheel stores filter aliases and focus offsets on-device** (`POAGetPWFilterAlias`, `POAGetPWFocusOffset`, settable via Player One's own software). The filterwheel driver seeds `Names`/`FocusOffsets` from the wheel at connect; `filterNames` from config (set via `set_names`) takes precedence. The driver does not write aliases/offsets back to the wheel.
- **Position while moving**: `POAGetCurrentPosition` returns `PW_ERROR_IS_MOVING` while the wheel is rotating. The wrapper's `get_position` checks `POAGetPWState` first and maps the moving window to `-1`, which is exactly the ASCOM `Position` contract — don't translate that SDK error into an exception on the read path.
- **`PW_ERROR_FIRMWARE_ERROR`** means filter position and hole are misaligned; the SDK doc says to call `POAResetPW` (exposed as `reset_wheel` in the wrapper) to recover.
- **udev rules are identical** between the camera and filter wheel SDKs (`99-player_one_astronomy.rules`) — the camera SDK's copy already installed by packaging covers both device types; don't install it twice.
- **PW SDK `.so` symlink chain**: upstream ships only `libPlayerOnePW.so.1.2.3`; the `libPlayerOnePW.so` / `.so.1` / `.so.1.2` symlinks needed for linking were created by hand in `lib/arm64/`. Recreate them on an SDK version bump.
- **Cooler / dew heater / fan (cooled models, e.g. Uranus-C PRO)**: the SDK exposes `POA_COOLER`, `POA_TARGET_TEMP`, `POA_COOLER_POWER` (standard ASCOM cooler surface, implemented in the camera driver) plus `POA_HEATER_POWER` and `POA_FAN_POWER` (percent, range from `POAGetConfigAttributes`). Use `POA_HEATER_POWER`; `POA_HEATER` is deprecated (read-only bool). `POA_COOLER` turns the cooler **and fan** on together; the TEC power closed-loops against `POA_TARGET_TEMP` (that's the read-only `POA_COOLER_POWER`), while the fan runs at whatever `POA_FAN_POWER` is set to — the fan does NOT auto-vary with temperature.
- **Dew heater / fan are runtime-only, by design (no persisted config)**: exposed via camera custom Actions (`GetHeaterPower`/`SetHeaterPower`/`GetFanPower`/`SetFanPower`) and a Switch device (`playerone_switch_driver`, "Player One Thermal Switch") with one multi-value element per control — the Switch is what gives NINA-style clients sliders. A connect-time `heaterPower`/`fanPower` config was implemented and deliberately removed: a persisted "heater on" set in December would silently re-apply every connect months later (wasted power, heat fighting the TEC, no client visibility). On power-up the camera uses its own firmware/SDK defaults; turning the heater on is an explicit per-session act. The cooler is intentionally NOT on the Switch — `CoolerOn`/`SetCCDTemperature` on the standard Camera interface are the single owner of cooling, same as ZWO.
- **SDK wrapper open/close is reference-counted** (mirrors `zwo_sdk_wrapper`): the camera and thermal switch devices share one `POAOpenCamera` handle; the camera is physically closed only when the last user disconnects. Any new Player One device type that opens a camera must go through the wrapper's open/close, never raw SDK calls.
- **Switch ID validation order**: out-of-range switch IDs throw `InvalidValue` even while disconnected (ASCOM contract, same as the ZWO dew heater switch). Since the element count is per-model (heater and/or fan) and only known after connect, the disconnected bound is the potential count and `MaxSwitch` reports 2 until connect refines it.
- **Uranus-C PRO validated on hardware (2026-06-12, IMX585)**: heater effect confirmed by calorimetry (full heater costs ≈10 points of TEC headroom at a held -10 °C target); firmware powers up with heater at 10%; camera + thermal switch ran concurrently in NINA on the shared refcounted handle.

### SynScan (SkyWatcher)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/SynScan/`. No external SDK required — uses serial communication directly.

Connection types: Serial (USB serial) only. Default 9600 baud, 8N1. Protocol versions V3 (older) and V4 (current).

- Auto-detection scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for SynScan hand controllers, probes each port with a firmware version query, and connects to the first responding mount.
- **Pulse guiding**: SynScan V3/V4 protocol has no hardware pulse guide command. Driver implements software-timed variable-rate slew: issues a variable-rate axis slew at the guide rate, sleeps for the requested duration, then stops the axis and restores sidereal tracking. `IsPulseGuiding` tracks completion via time-based end time plus delay.
- **GEM pier-side DEC direction flip**: DEC motor direction is inverted when the mount's pointing state is 'W' (west), matching the physical axis reversal on German equatorial mounts. This affects pulse guide and MoveAxis DEC commands.
- **Position override accumulation**: Instead of reading back noisy mount positions after tiny guide pulses, the driver accumulates expected `rate × duration` deltas directly into the target coordinate frame. All consecutive pulse guide directions (N/S/E/W) operate in the same coordinate baseline, eliminating drift between reads.
- **RA tracking restoration**: The stop thread re-issues `set_tracking_mode()` after stopping an RA-axis pulse to counteract the variable-rate stop command killing sidereal tracking. Without this, the mount stops tracking after every RA pulse guide.
- ConformU 4.3.0 validated for **Sky-Watcher HEQ5 PRO** on Linux arm64 with 0 errors and 0 issues.

### iOptron

Devices: Telescope (mount), Switch (iMate PowerBox).

Protocol documentation: `AlpacaCore/external/iOptron/RS-232_Command_Language2014V310.md`. No external SDK required — uses RS-232 serial communication directly.

Connection types: Serial (USB serial, 115200 baud default per v3.10 spec) and Network (WiFi TCP, default port 4030; varies by model — HEM27 uses 8899).

- Auto-detection scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for Prolific/FTDI/CP210x/Silicon Labs USB-serial adapters, probes each port with `:MountInfo#`, and connects to the first responding mount.
- **`:MountInfo#` quirk**: iOptron returns exactly 4 ASCII digit bytes with no `#` terminator. The protocol wrapper uses idle-timeout read mode (`require_hash_terminator=false`) for this command. Most other commands do terminate with `#`.
- **Model code table**: iOptron reassigned model codes between protocol v2 and v3 (e.g., code `0025` is HEM27 in v3, was CEM25 in v2). Use the INDI v3 driver's mapping, not the older Indigo-derived table.
- **SideOfPier**: The mount's `:GEP#` response includes a raw physical pier side value, but this does not match the ASCOM convention when tracking past the meridian. The driver computes SideOfPier from hour angle (LST − RA): `pierEast` (0) for HA ≥ 0, `pierWest` (1) for HA < 0. This matches the `DestinationSideOfPier` logic.
- **Sync**: Use the mount's `:CM#` command (after `:SRA#` and `:Sd#` to set target) to calibrate the mount's internal pointing model directly. Do NOT maintain driver-level sync offsets — they cause coordinate divergence during slews because the mount doesn't know about them. The `:CM#` approach keeps mount and driver in agreement.
- **Serial buffer flush**: Stale bytes from previous command responses can contaminate `:MS1#`/`:MS2#` slew responses (e.g., `"1111"` instead of `"1"`). The driver calls `flush_input()` (via `tcflush`/`PurgeComm`) before issuing slew commands.
- **Pulse guiding**: Uses native iOptron pulse guide commands (`:ZS#`, `:ZQ#`, `:ZE#`, `:ZC#` for N/S/E/W with duration in ms). Hardware-timed by the mount.
- **`:GEP#` response format**: sign + 8 RA digits + sign + 8 DEC digits + 1 side_of_pier digit + 1 pointing_state digit. No `#` terminator on some firmware versions — use idle-timeout read.
- ConformU 4.3.0 validated for **iOptron HEM27** on Linux arm64 with 0 errors and 0 issues; ConformU 4.4.0 validated for **iOptron HAE29C EQ** (USB) with 0 errors, 0 issues, 0 timing violations.
- **Blind commands are not blind** (HAE29C session, 2026-07-14): `:SG`/`:SDS`/`:SUT`/`:Q`/`:ST`/`:MP`/`:MH` all return a `1` ack on current firmware. `send_command_blind` drains it on BOTH transports (serial got its drain 2026-07-14; only Wi-Fi had one before) and `send_command` flushes stale input before every write — a busy mount can ack later than any fixed drain window, and one leaked byte shifts every fixed-offset field parse (`:GPC`/`:GLS`/`:GEP`). If a trace shows `RESP 11` or a `1-`-prefixed response, suspect a new blind command missing its drain.
- **HAE29C (model code 0036) firmware quirks** — all workarounds live behind `hae29c_quirks_active()` (strict equality on `0036`; HEM27/HAE43/other codes get stock behavior). Verified on hardware: (1) `:MP1#` completes physically but never reports status 6 — `:ST0#` finalizes it (driver watches for slewing-but-stationary-at-park-target); (2) zero-distance park wedges identically; (3) GOTO settles ~11–16 arcsec east in RA (final-approach sidereal gap) and re-GOTOs under ~15" deadband — the driver closes the residual with a duration-computed pulse-guide trim (RA only; Dec is accurate and Dec pulse polarity flips with pier side). If the same symptoms show up on HAE29C-EC/AA (codes 0037–0039), extend the gate — one line.
- **Stale status-cache traps**: `find_home()` must force-refresh before its already-home early-return, and `park()`/`unpark()` must invalidate cached `is_at_home` — a Park/Unpark cycle otherwise leaves a stale `is_at_home=true` that silently no-ops the next FindHome (ConformU: "AtHome reports false after FindHome").
- **ConformU 4.4 tests `Connect()` immediately after `Connected=false`** — an async disconnect must not drop a racing connect (AsyncConnectable now queues it; see `pending_connect_`). ConformU 4.3 never exercised this, so a 4.3 pass does not imply a 4.4 pass on the connect phase.
- **Debugging discipline from this session**: mount "wedges" that survive reconnects but clear on a fresh flushed port open are usually leaked-byte desync or firmware state, not dead hardware; a mount that answers probes but fails mid-motion points at the USB link (here: loose cable + VMware passthrough drop under motor load). Reproduce failing ConformU members individually with `curl` against the live server before burning full ConformU runs.

#### iMate PowerBox (Switch)

The iMate is iOptron's embedded astronomy computer (OrangePi 3 LTS / Allwinner H6, arm64). Its "PowerBox" accessory exposes switchable DC power ports. This is a **local GPIO** device, completely independent of the mount RS-232 protocol — AlpacaBridge runs *on* the iMate and toggles GPIO directly. It does **not** share `ioptron_protocol_wrapper`; it has a dedicated `ioptron_powerbox_wrapper` (libgpiod, `/dev/gpiochip1`).

> **The iMate now runs the OpenAstro Armbian image, not the stock iOptron OS.** The stock BSP kernel crashed under load (the `cpufreq_dt` OOPS when WiFi/BT powers on wedges the CPU governor, hanging AlpacaBridge before it can bind its port), so OpenAstro re-bases the iMate on Armbian's mainline kernel (Debian 13 Trixie) — see the aw-flashtool repo. **Consequence for this driver:** the H6 main pinctrl (`300b000`) is `/dev/gpiochip1` on mainline (the dead BSP exposed it as `gpiochip0`). Line offsets are unchanged. The historical WiringPi / stock-tooling notes below describe the *old* stock OS and are kept only for context.

- **Hardware**: 3 physical DC jacks. Two are GPIO-controllable, one is a hardwired always-on pass-through with no GPIO line:
  - `DC1` → gpiochip1 line **118** (PD22).
  - `DC2` → gpiochip1 line **114** (PD18).
  - `DC3` → always-on pass-through, no GPIO.
- **Switch mapping** (`MaxSwitch = 3`): switch 0 = `DC3 (always on)` (read-only, `CanWrite=false`, `GetSwitch` always true, writes throw `NotImplemented`); switch 1 = `DC1`; switch 2 = `DC2`. DC1/DC2 default boolean (min 0 / max 1 / step 1) and can each opt into soft-PWM (min 0 / max 100 / step 1) via the per-port `pwm_enabled` flag. DC3 is always boolean read-only.
- **Discovery of the control path**: stock iMate drives the ports via the setuid WiringPi `gpio` tool (`/usr/local/bin/gpio mode/write/read`) from `/home/imate/imatepowerbox.sh`, and configures them as outputs driven high at boot via `dc-power-ports.service` (`gpio mode 2/6 out; gpio write 2/6 1`). There is also a Dart `tcp_server.service` (port 3000) used by iOptron's own apps, but it is not required (and was inactive on the test unit). We use libgpiod directly for consistency with the ZWO ASIAIR switch driver rather than shelling out.
- **WiringPi → SoC GPIO mapping**: WiringPi pin numbers (2, 6) are NOT the libgpiod line offsets. Use `gpio readall` on the device to map wPi → GPIO (wPi 2 = GPIO 118, wPi 6 = GPIO 114). libgpiod addresses lines by SoC offset.
- **Boot-high preserve / never-power-cycle**: general rules; note libgpiod releases the line on `close()`, so keep AlpacaBridge connected for the session.
- **Soft-PWM (opt-in per port)**: general soft-PWM at a **50 Hz default** — this hardware is where the general "PWM frequency is the lever" finding was made. The GPIO→MOSFET stage chops cleanly, but there is **no usable hardware PWM**: `gpio readall` labels DC1 (wPi 2 / PD22) `PWM.0`, yet the stock WiringPi sunxi build rejects `gpio mode 2 pwm`, and DC2 (PD18) isn't a PWM pin at all. A PWM port reports max 100 and `set_switch` maps on→100. Library-free bench test: sysfs bit-bang (`/sys/class/gpio`, PD22=118 / PD18=114).
- **DC3 read-only write ordering**: general read-only-port rule (`NotImplemented` before the connection check).
- **Permissions**: the service user needs access to `/dev/gpiochip1` (root, or a `gpio`-style group with a udev rule). The OpenAstro image already creates a `gpio` group, adds the `alpacabridge` user to it, and installs a `KERNEL=="gpiochip[0-9]*", GROUP="gpio", MODE="0660"` udev rule — so on the shipped image this is already handled. (The old stock `gpio` tool sidestepped it via setuid; libgpiod does not.)
### Celestron (NexStar)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Celestron/nexstar_protocol_reference.md` (combined reference — RS-232 serial, AUX bus, GPS, and AlpacaBridge driver notes). Original sources preserved in same directory. No external SDK required — uses RS-232 serial communication directly.

Connection types: Serial (RS-232 on hand control base) and Network (WiFi bridge adapters, default TCP port 2000).

- Communication: 9600 baud, no parity, one stop bit. All responses terminated with `#`.
- Position encoding: hexadecimal fraction of a revolution. Standard commands use 16-bit (4 hex digits, ~19.8 arcsec precision), precise commands use 24-bit (6 hex digits + "00" padding, ~0.08 arcsec precision). Commands: `E`/`e` for RA/Dec, `Z`/`z` for Alt/Az.
- GOTO: `R`/`r` for RA/Dec, `B`/`b` for Alt/Az. Sync: `S`/`s` (firmware v4.10+).
- Tracking modes: 0=Off, 1=Alt/Az, 2=EQ North, 3=EQ South.
- **CGE/Advanced GT quirk**: firmware versions 3.01–3.04 swap EQ North (1) and EQ South (2). This was corrected in later firmware. TODO: detect and handle this if needed.
- Model IDs from `"m"` command: 1=GPS, 3=i-Series, 4=i-Series SE, 5=CGE, 6=Advanced GT, 7=SLT, 9=CPC, 10=GT, 11=4/5 SE, 12=6/8 SE, 14=CGEM, 20=Advanced VX, 22=Evolution.
- Slew commands use pass-through (`P`) to motor controllers: device 16 = AZM/RA motor, device 17 = ALT/DEC motor. Fixed rates 1–9 (0 to stop), variable rates encoded as arcsec/sec × 4 in high/low bytes.
- Timeouts: NexStar spec says up to 3.5 seconds worst case for pass-through commands. Driver uses 5-second default.
- Time/Location: binary format (not ASCII). Timezone stored as hour offset (256-zone for negative). Location sign: 0=North/East, 1=South/West.
- **Home**: Mounts with hardware home switches (CGX, CGX-L, CGE Pro) use `MC_LEVEL_START` (0x0B) on both axes followed by polling `MC_LEVEL_DONE` (0x12). FindHome is asynchronous — send commands, return immediately, poll via Slewing/AtHome. Note: `MC_SEEK_INDEX` (0x19) / `MC_AT_INDEX` (0x18) are for PEC worm gear index (RA only), NOT home. See protocol reference for details.
- **Side of pier**: GEM mounts report pier side via the HC `p` command (`W` → pierWest, `E` → pierEast). This is a direct query — no hour angle inference needed.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via AUX bus pass-through. The firmware times the pulse internally — no sleep, encoder snapshotting, or sync calls required. Cross-axis is frozen at pre-pulse value during the guide window; active axis returns computed `baseline + (rate × duration)` as a one-shot correction to avoid ConformU tolerance failures at high declinations where cos(DEC) amplification causes geometric noise.
- **Adaptive RA slew offset**: Driver learns a running average of RA undershoot across slews and pre-biases subsequent slews to compensate for the CGX-L's no-tracking-during-goto behavior (matches INDI's `SlewOffsetRa` pattern).
- **Post-slew tracking restoration**: Re-issues the top-level `T` set-tracking-mode command rather than a per-axis variable-rate passthrough, keeping the HC's internal tracking state coherent with the LCD readout.
- **Site/time write skip when aligned**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped (log warn, return success) when the mount is aligned, matching INDI's UpdateLocation/UpdateTime pattern. Writing these after alignment corrupts the HC's pointing model. Preserves ConformU property round-trip tests.
- **Pier-safety gate**: Accepts either a successful `SyncToCoordinates` in the current driver session OR HC-reported alignment (`J` command). HC workflow: power on → Switch Position → Location → Last Alignment → "CGX-L Ready".
- ConformU 4.3.0 validated for **Celestron CGX-L** on Linux arm64 with 0 errors and 0 issues.
- The NexStar serial protocol is nearly identical to SynScan — both derive from the same Celestron protocol family. The driver implementation follows the same pattern but with separate namespace and branding.

### Bisque (Paramount / TheSkyX)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Bisque/` (INDI reference driver and TheSkyX scripting API docs). No external SDK required — communicates via JavaScript commands over TCP to TheSkyX Pro.

Connection types: TCP only. TheSkyX acts as middleware between the driver and the actual mount hardware.

- Communication is via JavaScript command snippets sent to the TheSkyX TCP scripting server (default port 3040).
- Commands are strings terminated with `#`. Responses are prefixed with `|No error. Error = 0.` on success, terminated with `#`.
- Special case: Handshake (`ConnectAndDoNotUnpark`/`IsConnected`) returns just `1` with no prefix.
- Slew is async: set `sky6RASCOMTele.Asynchronous = true`, call `SlewToRaDec`, poll `IsSlewComplete`.
- Pulse guiding uses `sky6DirectGuide.MoveTelescope(dRA, dDec)` with arcsecond displacement.
- Open loop motion for MoveAxis uses `DoCommand(9, 'direction|rate')` and `DoCommand(10, '')`.
- Park uses `ParkAndDoNotDisconnect()` to keep TCP connection alive (not `Park()` which disconnects).
- Pier side is read-only via `DoCommand(11, 'Pier Side')` — returns 1 for west of pier, else east.
- Find Home uses `FindHome()` with a 60-second timeout.
- Slew speed presets: 9 rates (1x, 2x, 4x, 8x, 32x, 64x, 128x, 256x, 512x sidereal).

### Astroasis

Devices: Focuser (Oasis Focuser).

Protocol: reverse-engineered USB HID vendor protocol (VID:PID `338F:A0F0`, 65-byte reports: 1 report-ID byte + 64-byte payload) recovered from decompiling/disassembling the vendor's ASCOM driver installer (`OasisFocuser64.dll` and its native SDK) — no vendor SDK binary is extracted, redistributed, or linked. See `AlpacaCore/external/oasisastro/README.md` for the recovered command reference. Talked to directly via `hidapi`'s hidraw backend — no serial port involved.

- Config takes either `hidPath` (explicit HID device path — lazy, does not touch hardware until `connect()`) or `focuserIndex` (0-based index into `enumerate_astroasis_focusers()`). **`create_astroasis_focuser_by_index()` scans the USB bus eagerly at construction time** and throws `NotConnected` immediately if no device is found — unlike SDK-index vendors (ZWO/QHY) whose index-based constructors are hardware-free. This means a no-hardware routing/config test must use the explicit `hidPath` form; the auto-index path can't round-trip without a real device attached (see `test_routing.cpp`'s astroasis case).
- `hidPath` takes precedence over `focuserIndex` when both are present — router checks `hidPath.empty()` first.
- The onboard temperature sensor is an NTC thermistor read through a 12-bit ADC and converted via a Steinhart-Hart-style curve ported byte-for-byte from the vendor DLL's data section (see `raw_adc_to_centidegrees()` in `astroasis_protocol_wrapper.cpp`) — the formula constants have not been cross-checked against a real temperature reading, only against the DLL's own math.
- `StepSize` and temperature compensation are not exposed by the device/protocol — both throw `PropertyNotImplemented`, don't try to synthesize a value.
- ConformU 4.4.0 validated on Debian 13 arm64: **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/Astroasis/Oasis Focuser/`.

### Gemini

Devices: Focuser (Automatic Astro Focuser Pro), CoverCalibrator (Astro Flat Panel Cover Lite).

#### Automatic Astro Focuser Pro (Focuser)

Protocol: MyFocuserPro2 serial protocol (Arduino-based). Reference docs in `AlpacaCore/external/Losmandy/` (Gemini L4 command set). No external SDK required.

Connection types: USB serial (CH340/CH341 adapters) only. No WiFi support.

- **DTR/HUPCL quirk**: CH340 USB-serial adapters assert DTR on port open, which resets the Arduino/ESP32 MCU. During auto-detect probe, the driver clears HUPCL before closing the port so DTR stays high. On the subsequent connect, the MCU does not reset again. Do not remove this HUPCL handling — it prevents a ~4 second double-boot penalty and possible connection failure.
- Auto-detection scans `/dev/serial/by-id/` for CH340/CH341 chips (USB vendor `1a86`), falls back to `/dev/ttyUSB0` through `/dev/ttyUSB9`.
- Connection uses 3 retry attempts with staggered wait times (100ms, 2s, 1s) — worst case ~9.1 seconds, designed to stay under the ASCOM client 10-second timeout.
- Motor speed is forced to fast (`:1502#`) on connect so moves complete within ConformU's 60-second timeout.
- Temperature is forced to Celsius (`:16#`) on connect — do not assume the focuser's default unit.
- `get_step_size()` throws `PropertyNotImplemented` — the MyFocuserPro2 protocol does not expose step size in microns; it varies by mechanical configuration.
- Movement commands (`:05<pos>#`) are fire-and-forget (blind). Poll `is_moving()` (`:01#`) to detect completion.
- Some commands are blind (no response). Use `send_command_blind_locked()` for these to avoid blocking on a timeout waiting for data that will never arrive.
- Default baud rate is 9600 (8N1). Configurable: 9600, 19200, 38400, 57600, 115200.
- ConformU validated (v4.2.1) on Debian 13 arm64. Results in `AlpacaCore/conformu/Gemini/`.

#### Astro Flat Panel Cover Lite (CoverCalibrator)

Light-only flat panel — no motorized cover. Not the same MCU family as the focuser above: don't assume shared protocol or auto-detect matching.

Protocol: no SDK or published spec exists for this model. Reverse-engineered entirely from the vendor's Windows control app (traffic capture). Treat the parsing notes below as the ground truth for this driver, not an external reference doc — there is nothing else to cross-check against except real hardware.

- **Command/reply shape**: ASCII `>X#` (query) / `>Xnnn#` (set) at 9600 baud 8N1. Replies are `*` + the echoed command letter + payload + `#` (e.g. `>V#` → `*V206#` for firmware 206). **`>S#` is the one exception**: it replies `*S<d1><d2><d3>#` — three single-digit flags, not one combined number. `d1` is the light on/off flag; `d2`/`d3` stayed `"1"` across every light/brightness change observed on the test unit and are presumed cover-related flags that don't apply to this motorless model. Parse `d1` directly (the digit right after `S`) — don't run the generic trailing-integer parser against `>S#` replies, since `"*S011#"` would parse as `11` (nonzero → wrongly "on").
- **`>H#` handshake reply must be held in full**: confirmed on hardware as `*HGeminiFlatPanelLite#` (22 chars) — a too-small read buffer truncates before the `#` terminator and the port gets wrongly rejected as a non-match during auto-detect.
- **Auto-detect's `probe_port()` must check the reply *content*, not just that it's `#`-terminated** (PR #143 review, joeytroy): the candidate by-id patterns (CH340/CH341/`USB_Serial`/`1a86`) are exactly what the Gemini focuser also enumerates as, and the focuser's MyFocuserPro2 firmware answers unrelated queries with its own `#`-terminated replies. With both devices plugged in, accepting any well-formed reply let `probe_port()` misidentify the focuser's port as the flat panel. Fix: require the known `*H` reply prefix before accepting a port as a match. The connect-time handshake requires the same `*H` prefix (second review round): a manually configured `portPath` is the one path that never went through the probe, and pointing it at the focuser's port would otherwise "connect" to the wrong device and parse garbage from every later `>S#`/`>J#` reply — every `>H#` reply check goes through `is_flatpanel_handshake_reply()`, discovery and connect alike.
- **Controller is an ESP32-class board on Espressif's native USB-serial/JTAG stack** (by-id name `usb-Espressif_USB_JTAG_serial_debug_unit_...`), **not** a CH340/CH341 adapter like the focuser. Auto-detection scans `/dev/serial/by-id/` for CH340/CH341/generic `USB_Serial`/vendor-`1a86` names *and* `Espressif`, then probes with the `>H#` handshake; falls back to `/dev/ttyUSB0`–`9`. If a future revision uses a different USB bridge chip, add its by-id pattern rather than assuming the CH340 quirk applies here too — it might not need the DTR/HUPCL workaround at all on native USB, but the driver applies it defensively anyway (see next point) since it's cheap and matches the focuser's proven-safe behavior.
- **DTR/HUPCL quirk applied defensively**: same HUPCL-clear-before-close as the Gemini focuser and WandererAstro, to keep DTR high across port close and avoid a possible MCU reset on reopen. Probe waits up to 2s (first attempt) + 1s (retry) for a post-reset boot before the `>H#` handshake.
- **No motorized cover**: `OpenCover`/`CloseCover`/`HaltCover` throw `MethodNotImplemented` unconditionally (do not follow the WandererCover pattern of making `HaltCover` function — that rule applies to CoverState-capable covers; this model's `CoverState` always reports `NotPresent`, so ConformU never requires `HaltCover` to work).
- Default baud rate 9600 (8N1); configurable 9600/19200/38400/57600/115200 same as the focuser, though only 9600 has been confirmed against real hardware.
- ConformU 4.4.0 validated on Debian 13 arm64: **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/Gemini/Astro Flat Panel Cover Lite/`.

### WandererAstro (WandererCover V4, WandererRotator Mini, SFW filter wheels, WandererBox Pro V3)

Devices: CoverCalibrator, Rotator (the project's first — the generic CoverCalibrator dispatch already existed in `AlpacaHTTP/src/http/router.cpp`; only the vendor instantiation block + web UI were new). The WandererCover V4 is a motorized dust cover combined with an EL flat panel.

Protocol: ASCII serial. Docs in `AlpacaCore/external/WandererAstro/` (`Serial Protocol for WandererCover V4-EC and V4-EC IR.md` + the V4 user manual PDF). INDI reference: `drivers/auxiliary/wanderer_cover_v4_pro_ec.cpp` in indilib/indi. No external SDK.

Connection types: USB serial (CH340 adapter, vendor `1a86`) only. No WiFi. Fixed **19200 baud, 8N1**.

- **Streaming status, not request/response**: the cover continuously transmits a `\n`-terminated, `A`-delimited status frame (~1 Hz): `<model>A<firmware>A<closePos>A<openPos>A<curPos>A<voltage>A<brightness>A<dewHeater>A<asiair>`. The protocol wrapper runs a **background reader thread** that keeps the latest parsed frame, so every Alpaca property read is served from cache and never blocks on serial I/O (keeps CoverState/CoverMoving/Brightness inside ConformU's FAST 0.1s budget).
- **Commands are fire-and-forget** ASCII numbers with a trailing `\n`: open `1001`, close `1000`, brightness `1`–`255`, light off `9999`. There is no reply to commands — never wait for one.
- **Device identity**: the model token begins with `WandererCoverV4` (bench unit reports `WandererCoverV4Pro`; the EC variant differs only in the token). Auto-detect and connect both match on this prefix. Connect waits up to `serial_timeout_s` (3s) for the first identifying frame, else throws `NotConnected`.
- **CoverState has no "done" signal**: the cover reports only its live angle. Like INDI, treat the cover as having reached its target when the current angle is within **±10°** of the configured open/close angle. The driver tracks the last commanded direction (Opening/Closing) to distinguish Moving from Open/Closed; at rest with no command and not near either set point it reports `Unknown` (a valid ASCOM state).
- **HaltCover must FUNCTION (not throw `NotImplemented`).** ConformU treats a cover-capable device (CoverState ≠ NotPresent) as required to implement `HaltCover` — throwing `NotImplemented` is logged as an issue ("this method must function per the ASCOM specification"), and ConformU then opens the cover and calls `HaltCover` mid-move, requiring `CoverState`/`CoverMoving` to report *not moving* afterward. The WandererCover protocol has no halt command, so `HaltCover` instead clears the driver's commanded-move target: `CoverState` immediately stops reporting `Moving` (becomes position-derived `Unknown`/`Open`/`Closed`) while the cover finishes its travel mechanically and the controller stops the motor at the end stop. It sends no serial command. It does require a connection (throws `NotConnected` disconnected).
- **CalibratorOn brightness range is [0, 255]**; out-of-range → `InvalidValue`. The range is validated *before* the connection check so the boundary is testable without hardware (equivalent when connected, which is how ConformU exercises it). The panel applies brightness instantly, so `CalibratorChanging` is always false.
- **CalibratorState/Brightness must be synchronous, NOT read from the status stream.** ConformU reads `CalibratorState` and `Brightness` *microseconds* after `CalibratorOn`, but the device's streamed status lags a command by up to ~1s — so deriving them from the stream returns the stale previous value and ConformU fails with "CalibratorState was 'Off' instead of 'Ready'" (and the next `CalibratorOff` fails as a side effect, because ConformU reuses the field left by the last `CalibratorOn`). Fix: the driver tracks commanded brightness + an "engaged" flag, set synchronously in `calibrator_on`/`calibrator_off`; `get_brightness` returns the commanded value and `get_calibrator_state` returns `Ready` when engaged else `Off`. Seed these from the first streamed frame at connect (in case the panel was already lit). Note `CalibratorOn(0)` is "on at brightness 0" → `Ready` (engaged), even though the wire command is the off code `9999`.
- **CH340 DTR/HUPCL quirk**: same as Gemini — clear HUPCL so DTR stays high across close, avoiding an MCU reset on reopen. Shared in `configure_serial_fd()`.
- **Dew heater / input voltage / ASIAIR fields** are parsed from the status frame but are **out of ASCOM CoverCalibrator scope** and intentionally not exposed (consistent with the runtime-only thermal-control policy). Could later surface via `Switch` or `Action` if a user needs them.
- Interface version is **2** (ICoverCalibratorV2 — CoverMoving, CalibratorChanging, Connecting, DeviceState all implemented). A default `get_device_state()` was added to the `CoverCalibratorDriver` base so all future CoverCalibrator vendors share it.

**WandererRotator Mini (V1/V2)** — Rotator, `wandererastro_rotator_driver` + `wandererastro_rotator_protocol_wrapper`. Protocol reference: INDI `drivers/rotator/wanderer_rotator_mini.cpp` + `wanderer_rotator_base.cpp` (the INDI "Mini" driver explicitly covers V1 AND V2 — there is no separate V2 driver; we prefix-match the handshake token on `WandererRotatorMini` in case V2 firmware reports a suffixed name). USB serial only (CH340, 19200 8N1); **DC power is separate from USB** — the serial link and handshake work with DC absent, but moves silently do nothing (INDI logs "Rotator not powered!"), which is why the move monitor warns and estimates on completion-report timeout.

- **Request/response, unlike the streaming WandererCover**: nothing is sent unsolicited. Handshake `1500001` → `<name>A<firmware>A<angle*1000>A<backlash>A<reverse>A` ('A'-terminated fields). Firmware is YYYYMMDD; minimum supported 20240226 (older firmware predates this command protocol) — connect fails below it.
- **Command terminators are inconsistent by design**: handshake, move commands, and `Stop` are written as bare strings; config commands (reverse `1700001`/`1700000`, set-zero `1500002`, backlash `(deg*10)+1600000`) get a trailing `\n`. This mirrors the INDI reference byte-for-byte; the MCU parses both, but don't "clean it up".
- **Moves are relative, encoded as `(deltaDegrees × 1142) + 1000000`** (1142 steps/degree for the Mini; the Lite models differ). The controller is **silent during the sweep and reports the final `angle*1000` only at completion** (or after `Stop`) — so `IsMoving`/`Position` are served from a per-move monitor thread + time-extrapolation at ~1°/240 ms, never from a blocking serial read. Flush the input buffer before issuing a move: a stale completion report would fake an instant arrival.
- **The reported mechanical angle is signed and accumulates**; the position is its magnitude / 1000. If |raw| > 400000 the INDI reference resets it to zero via `1500002` and re-handshakes; ours does the same (threshold 400 for V2's degrees format, below).
- **V2 firmware breaks the INDI angle conventions — detect the format by the token text** (found via ConformU 2026-07-23 on real Mini V2 hardware; all 23 issues were Position ≈ target/1000): V1 reports angles as bare-integer (degrees × 1000) and the move-completion report is the ABSOLUTE accumulated angle; **V2 reports decimal degrees (token contains a '.') and the move-completion report is the RELATIVE distance travelled (magnitude)**. The wrapper branches on `token.find('.')` in both the handshake parse and the completion monitor, and reconstructs the V2 absolute angle as `move_start + copysign(report, commanded_delta)`. The INDI "Mini V1/V2" driver divides by 1000 unconditionally, so it presumably mis-tracks position on V2 firmware — do not "fix" our parser back to match INDI.
- **Sync is a driver-side offset** (IRotatorV4 semantics). Do NOT use the device's `1500002` set-zero for Sync — it destroys the mechanical coordinate.
- **Never discard partial bytes on a read-window timeout** (found via ConformU round 2: 7 issues, positions off by exactly the report's lost leading digit/sign — "136.02"→"36.02", "-40.00"→"40.00"): the completion monitor polls in short `read_section` windows for the whole multi-second move, and a report arriving astride a window boundary lost the characters read in the timing-out window. `read_section` takes a `std::string& carry` that preserves partial tokens across calls; clear it whenever the wire is flushed. This applies to any polled-window serial reader.
- The V2 completion report is **signed** relative decimal degrees (e.g. `-180.00` after a −180° move); the wrapper reconstructs absolute as `start + copysign(|report|, commanded_delta)`.
- The in-use serial-port registry is now shared across wrappers (`util/serial_port_registry.h`, promoted from the cover wrapper) so a rotator auto-detect probe never injects its handshake into a port the connected cover holds, and vice versa. Any future serial vendor should use it too.
- ConformU 4.4.0 validated on real hardware (WandererRotator Mini V2, firmware 20250222, Debian 13 arm64): **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/WandererAstro/WandererRotator Mini V2/Linux-arm64.txt`. Took three rounds: round 1 (23 issues) exposed the V2 decimal-degrees/relative report format, round 2 (7 issues) exposed the read-window partial-token loss — both documented above.
- ConformU 4.3.0 validated on real hardware (WandererCover V4 Pro, firmware 20250504, Debian 13 arm64): **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/WandererAstro/WandererCover V4/Linux-arm64.txt`. First clean run had 7 issues (6× calibrator-state lag, 1× HaltCover NotImplemented) — see the two notes above; both are general ASCOM CoverCalibrator lessons, not WandererCover-specific.

**SFW filter wheels (SFW50 / SFW50S / SFW36S)** — FilterWheel, `wandererastro_filterwheel_driver` + `wandererastro_filterwheel_protocol_wrapper`. Protocol reference: INDI `drivers/filter_wheel/wanderer_snowflake.cpp`. The whole lineup is 8-slot; the wire model tokens are `WSFW508` (SFW50/SFW50S) and `WSFW368` (SFW36S). USB serial only (CH340, 19200 8N1); **12 V DC is separate from USB** — the serial link and status stream work with DC absent, but moves silently do nothing.

- **Streaming like the cover, but token-based, not line-based**: the wheel streams an 'A'-delimited frame `<model>A<firmware>A<position 1-8>A<letters>A<8 per-filter fields>A<deviceID>A`, and **line terminators may be omitted entirely while the wheel is moving** (documented in the INDI reference). A line-based reader like the cover's would stall mid-move, so the wrapper parses a rolling 'A'-delimited token stream that resynchronises on each `WSFW` model token. Filter letters are restricted to B–Z by the vendor precisely so 'A' stays unambiguous as the delimiter.
- **Commands are '\r'-terminated** (the cover uses '\n') fire-and-forget ASCII numbers: move `2000+slot` (1-based), auto-calibrate `1500002` (note: same code the ROTATOR uses for set-zero — same vendor, different meaning per device), zero-detect `1002`, set slot letter `(161+i)*10000+(letter-'A'+1)`, set device ID `1900000+id`. Only move and calibrate are used; letters/device-ID are out of ASCOM scope.
- **Minimum firmware 20260124** (enforced at connect, like the rotator's floor) — older firmware predates the streamed-status protocol; update via WandererEmpire.
- **Homing at connect**: calibrate (`1500002`) is sent once per connect, matching INDI. Fire-and-forget with no move target recorded — Position reports the live streamed slot during the home. ConformU 4.4.0 is untroubled by this.
- **Position -1 while moving**: the driver records the commanded target; Position returns -1 until the streamed position matches, then latches idle (so a later out-of-band position change reads as the live slot, not a stuck move). Slot count is statically 8 for the whole lineup, so the full Position range is validated as InvalidValue BEFORE the connection check, and names/offsets state is built at construction.
- Filter Names/FocusOffsets are driver-side (config-persisted) like ZWO/PlayerOne — the on-device single-letter names are parsed from the stream but not used or written back.
- ConformU 4.4.0 validated on real hardware (SFW36S, firmware 20260124, Debian 13 arm64): **0 errors, 0 issues, 0 timing issues** on the first run. Results in `AlpacaCore/conformu/WandererAstro/SFW36S/Linux-arm64.txt`.

**WandererBox Pro V3 (power box)** — Switch, `wandererastro_box_switch_driver` + `wandererastro_box_protocol_wrapper`. Protocol reference: INDI `drivers/auxiliary/wandererbox_pro_v3.cpp`. USB serial only (CH340, 19200 8N1). The vendor's WandererEmpire app and their own ASCOM driver were used as the surface reference (screenshots verified 2026-07-26).

- **Streaming, token-based like the SFW**: continuous 23-field 'A'-delimited frame: `ZXWBProV3`A`<fw>`A`<probe1>`A`<probe2>`A`<probe3>`A`<DHT hum>`A`<DHT temp>`A`<total A>`A`<19V A>`A`<DC3-4 A>`A`<input V>`A`<USB3.1-1>`A`<USB3.1-2>`A`<USB3.1-3>`A`<USB2 1-3>`A`<USB2 4-6>`A`<DC3-4>`A`<DC5>`A`<DC6>`A`<DC7>`A`<DC8-9>`A`<DC10-11>`A`<DC3-4 set×10>`. All values raw except the DC3-4 setpoint (÷10). Identity must be exactly `ZXWBProV3` — the Plus V3 (`ZXWBPlusV3`) is a DIFFERENT port layout with its own INDI driver; reject it rather than mis-mapping ports. Dew point is NOT in the frame — compute via the Magnus formula from the DHT22 pair (matches INDI/vendor app).
- **Commands** are `\n`-terminated fire-and-forget: DC3-4 `101`/`100`, DC8-9 `201`/`200`, DC10-11 `211`/`210`, USB3.1-1/2/3 `11x`/`12x`/`13x`, USB2.0(1-3) `14x`, USB2.0(4-6) `15x`, PWM `<ch><%03d>` (ch 5/6/7, 0-255), DC3-4 voltage `20<%03d>` (volts×10, floor 5.0V per INDI/vendor UI), current calibration `66300744` (not exposed via Alpaca).
- **Switch surface**: 24 ids — outputs 0-13 (vendor ASCOM driver ordering: DC1/DC2 read-only always-on gauges first), sensors 14-23 as READ-ONLY switches. Sensors can't go in `DeviceState` (the `SwitchDriver` base `get_device_state()` is `override final` and only reports per-switch members), so read-only switch values are the pattern for sensor telemetry. Sensor [Min,Max] ranges must cover error sentinels (DS18B20 unconnected probes report -127/85 °C — range floor -273.15 covers both) or ConformU's value-in-range check fails.
- **Commanded-value semantics on writable outputs** (cover/ETA lesson): the frame lags a command by up to a frame period, ConformU reads back microseconds after writing. Writes quantise to the switch step before commanding/recording.
- **Firmware floor 20240216 is a WARN, not a reject** (INDI behaviour): older firmware only lacks calibrated current readings.
- **Firmware streams literal `nan` for an absent DHT22** (found via ConformU round 1, firmware 20250410): `std::stod("nan")` parses successfully to NaN, nlohmann::json serializes NaN as JSON `null`, and ConformU's DeviceState parser crashes on it ("Object reference not set"). Sanitize ALL non-finite sensor values at frame parse (temps/dew point → -127, humidity/power → 0). General lesson for any driver exposing parsed floats via DeviceState or switch values.
- **Step-quantisation FP accumulation** (ConformU round 1): `min + round((v-min)/step)*step` at v=Max produced 13.200000000000001 > 13.2 and failed the wrapper's range check exactly at the boundary ConformU tests. Clamp the quantised value into [min, max].
- ConformU 4.4.0 validated on real hardware (WandererBox Pro V3, firmware 20250410, Debian 13 arm64): **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/WandererAstro/WandererBox Pro V3/Linux-arm64.txt`. Round 1 had the two issues above; round 2 was clean.
- **`switchType` discriminator** (`wandererbox-pro-v3`) in the router config from day one — the parked ETA tilt adjuster branch will add `eta` as a second backend under (wandererastro, switch); dew-heater auto modes (dew-point/constant-temp) stay device-side per the runtime-only thermal policy (the vendor's own ASCOM driver also only writes Manual Mode).

### WeeWX

Devices: ObservingConditions.

No external SDK — reads weather data from a local WeeWX weather station instance.

## General Notes

- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- ConformU logs live under `AlpacaCore/conformu/`.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add desktop GUI frameworks (Qt, GTK, wxWidgets, etc.). The web UI in `AlpacaHTTP/web/` is the only user interface.
- Do not invent device types outside the ASCOM Alpaca standard set (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope). Shutter control is part of the **Dome** interface (`OpenShutter`/`CloseShutter`/`ShutterStatus`), not a standalone device. A non-standard `Shutter` device type existed as unused scaffolding and was removed 2026-06-09 — clients (NINA, ConformU) cannot consume non-standard types, so they break interoperability.
