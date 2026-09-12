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
- **ConformU 4.5.0 has a known arm64 timing bug — use 4.5.1+ (currently a beta download, not yet a GitHub release).** On arm64, the *first* Camera-device member returning each distinct .NET response type (`CameraState`/enum, `CameraXSize`/int, `SensorType`/enum) in a fresh ConformU process is charged ~0.13-0.22 s "OUTSIDE FAST RESPONSE TIME TARGET", while AlpacaBridge answers in <1 ms (confirmed via TRACE-level dispatch timing AND a loopback packet capture during SC715C validation, 2026-09-11: the request/response round trip was 3 ms; the ~140 ms was entirely client-side, between ConformU receiving the response and issuing its next request). Root cause: the official `conformu.linux-arm64.tar.xz` 4.5.0 release was accidentally published without `PublishReadyToRun`, so .NET JIT-compiles each new generic-over-value-type instantiation (`TimeFunc<CameraState>`, `TimeFunc<int>`, `TimeFunc<SensorType>`) on first use — reproducible on ANY vendor's camera driver on this rig (confirmed identically on ZWO ASI120MM Mini), not a driver bug. Filed and fixed upstream: [ConformU#31](https://github.com/ASCOMInitiative/ConformU/issues/31); the maintainer's 4.5.1 beta (rebuilt with `PublishReadyToRun`) is clean. Get it from `https://download.ascom-standards.org/beta/conformu.linux-arm64.tar.xz` until a formal 4.5.1 GitHub release exists — do NOT validate a driver's timing against a 4.5.0 arm64 run; a "3 members took longer than their target response times" result showing exactly these three members (and nothing else) is this bug, not a regression to chase in driver code. The same class of bug hit Telescope devices too (`AlignmentMode`, `EquatorialSystem`, `SideOfPier` — first use of each enum type).

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

**ConformU rate-offset tests and the position model (PR #221, 2026-08-25).** ConformU measures
`RightAscensionRate`/`DeclinationRate` by sampling RA/Dec BEFORE the rate write and 10 s after it,
with a 5% tolerance — at the 0.05 arcsec/s low rate that is 0.025 arcsec over 10 s. Any position
discontinuity inside the rate setter fails it: a hardware re-anchor (`refresh_position_cache_locked(true)`)
on a MOVING axis shifts the reported position by up to one encoder count (~0.31 arcsec on the Wave
100i) plus start latency, which read as a 27% RA "rate" error, while the stationary Dec axis passed.
Rate setters must re-anchor the dead-reckoning model in place (`anchor_model_locked()`), never on
hardware. Also: ConformU runs ON the SBC over localhost — the dev VM's LAN path has 2-90 ms spikes
that stamp constant `Can*` getters with 0.10x s FAST marks — and a Bash tool timeout kills a child
ConformU mid-slew, so launch it detached (`setsid nohup`) and poll a done marker. Motor-controller
mounts store no site: set SiteLatitude/Longitude in the device config first, or the driver
refuses the connect (#274) and ConformU never reaches CheckMethods.

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
  The round-trip test (Required Test Case #9) is the automated catch.

> The connection-thread lifecycle lives in ONE place: `AsyncConnectable`
> (`AlpacaCore/include/alpacacore/async_connectable.h`). Every vendor driver
> inherits it (issue #100); a new driver that copy-pastes its own
> `connection_thread_` machinery is a review-blocking regression.

> A driver that refuses a connect should say why in the exception it throws:
> since #358 `AsyncConnectable` keeps that text and the router reports it to
> the client instead of a bare "Connection failed", so the message is read by
> an operator in NINA, not only in the log. Write it for someone standing at
> the mount — name the setting to change, not the internal state that was
> wrong — and **never interpolate a credential or token into it**, because it
> is now a client-facing string on an unauthenticated LAN surface, not a log
> line. It also reaches the web UI as `LastConnectError` on
> `/management/v1/configureddevices`, which is the only place the Platform 7
> `PUT /connect` path can surface a reason at all.
>
> Every driver that mixes in `AsyncConnectable` must carry
> `ALPACA_EXPOSE_CONNECT_ERROR()` in a public section: it forwards the new
> `AlpacaDriver::get_last_connect_error()` virtual to the mixin's stored
> string. The router cannot reach the mixin by `dynamic_cast`, because the
> base is inherited `protected` everywhere and a cross-cast only traverses
> **public** base paths — such a cast compiles, always returns `nullptr`, and
> silently drops every reason. A driver that omits the macro compiles and
> tests green while reporting nothing, so `scripts/check_connect_error_hook.py`
> gates it in CI and in `ci_preflight.sh`.

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

Trusted fork contributors are also listed in the workflow's `allowed_non_write_users`
input (specific usernames, never `*`): without it, a fork contributor's own push to a
labeled PR fails the review job in seconds ("Actor does not have write permissions").
To onboard a new fork contributor, add their username to that list — do **not** grant
them collaborator/write access; the `safe-to-review` label stays the per-PR trust gate.

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

### ConformU telescope runs: bare mount only (SAFETY)

Never run the ConformU telescope suite with an OTA mounted. ConformU 4.5 commands 40+
maximum-rate slews (extended rate-offset tests at HA +/-9 and +/-3, MoveAxis at the full
AxisRates maximum, extended pulse-guide slews, SideOfPier model tests alternating across the
meridian), deliberate mid-slew aborts, and targets placed halfway to the horizon; recorded
mid-slew arcs on a Wave 100i dipped to ~5 degrees altitude. A mounted scope risks pier
strikes, cable snags, and balance failures; strain-wave mounts have no clutch to slip.
Validate on a bare mount, always. (The large swings during a run are ConformU's designed
choreography, not a driver fault — verify by checking that every slew lands on target.)

### Telescope Park / MoveAxis(axis, 0) are asynchronous initiators (all drivers)

ConformU 4.5 times `Park` (and every ITelescopeV4 initiator) against the 1 s STANDARD
target and completes it by polling `AtPark`/`Slewing`. Never block through a park slew.
The proven shape (SkyWatcher, then SynScan / Celestron in issue #208; Bisque pending on `fix/bisque-async-park`): reap the
slew task, snapshot the park target under the mutex, publish `slewing_cached_ = true` and a
`parking_` flag, then dispatch the slew + completion poll + tracking stop in the joinable
task thread, releasing the mutex between polls (`task_wait_for`, cancellable). `AtPark` and
`Slewing` flip in the same locked step (the public `Slewing` getter returns true while
`parking_`; the task polls the hardware through a separate `poll_hardware_slewing_locked`).
Park twice is a no-op; `Unpark`/`AbortSlew` during a park clear `parking_` (Unpark also
stops the axes and joins the task); disconnect and the destructor cancel + join. Keep any
pre-slew safety gate (Celestron alignment check) synchronous in `park()` so a refused park
still throws instead of silently never reaching `AtPark`. Drivers with native park commands
that return at once (iOptron `:MP1#`, OnStep `:hP#`, ZWO `:hP#`) need no task — verified
on the iOptron HAE29C with ConformU 4.5.0 (Park 11.5 s of motion, initiator well under 1 s).
`MoveAxis(axis, 0)` must likewise send the stop and return; only add a background
stop-completion task (SkyWatcher) when the mount has a deceleration ramp longer than the
target. Hardware-free coverage lives in `tests/test_<vendor>_async_park.cpp` over
`FakeMountServer`: the fake must answer the alignment/position probes the driver gates on
(Celestron `J` → `1#`, SynScan/NexStar `e` → a parseable pair with Dec < 90°), or the park
is refused before the GOTO is ever sent.

### TargetRightAscension and TargetDeclination are independent (all telescope drivers)

ASCOM treats the two target properties as separate: each getter must throw `ValueNotSet`
until **that property itself** has been written, and ConformU reports the shared-flag
version as "Read before write should generate an error and didn't" on both. Every driver
therefore carries `target_ra_set_` **and** `target_dec_set_`, each set only by its own
setter. One flag for both is a review-blocking regression — it was the original shape in
all seven drivers and took two passes to remove (#304, then #346).

The paths that legitimately define both coordinates at once set or clear both: the slew
and sync *coordinate* forms, any target seeding from the mount's own position (SynScan's
pulse-guide accumulator), the post-slew position-override and arrival reads, and the
connect/disconnect resets. `SlewToTarget`, `SlewToTargetAsync` and `SyncToTarget` require
the pair and must check it — Celestron and SynScan were both missing that check on the
synchronous form, which one shared flag hid, since any target write made it pass.

Hardware-free coverage per driver: read each property before any write, write RA alone and
check Dec still throws while RA reads back, confirm the three `*ToTarget` calls refuse the
half-set pair, then write Dec and expect both. A driver whose setters write to the mount
(iOptron) needs the fake rather than a disconnected instance.

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

### Cache-backed reads must track link health (issue #237)

A driver whose reads are served from a cache that a background reader fills (streamed status
frames, or a reader thread that re-polls on staleness) has a failure mode ConformU never sees:
the serial link dies (USB re-enumeration, unplugged cable, port stolen) and the cache is served
unchanged forever. The PDH ADV3 served byte-identical voltage/humidity for 30 minutes with
`Connected` true, and only a write surfaced the truth as `EIO`; the failed poll was a DEBUG log.
Rules, applied to every cache-backed serial driver (Gemini PDH, WandererBox/Cover/SFW):

- **Tie cache validity to the link.** A status-frame cache is only as good as the link that
  fills it. Latch a *link fault* after a small threshold of silence (PDH: 3 consecutive `>G#`
  polls with no frame, ~6 s; streaming Wanderer devices: 10 s without a frame via
  `util::StreamLinkHealth`), clear `valid` on the cached state, log the latch at ERROR (with
  the last `read()` errno when there was one) and the recovery at INFO. Never leave the only
  reaction to a failed poll write at DEBUG.
- **Refuse to serve a faulted cache.** Value reads AND writes throw `DriverException`
  ("<device> communications compromised: <reason>", the iOptron `device_faulted_` vocabulary),
  *commanded values included*: "what we last asked for" is no more trustworthy than the stale
  frame once the device is unreachable. Use `DriverException`, not `NotConnected`: `Connected`
  stays true (the client decides whether to reconnect) so `NotConnected` would contradict it.
  Where ASCOM has a word for "unknown" (`CoverState`/`CalibratorState::Unknown`) return it
  instead of throwing on the read; commands still throw.
- **Static metadata keeps answering** (names, descriptions, ranges, `CanWrite`, driver-side
  filter names/offsets) — it does not depend on the device. `DeviceState` then degrades to
  `TimeStamp` only through the base class's per-id try/catch.
- **Recovery is automatic**: keep polling/reading at the normal cadence while faulted so the
  first frame clears the latch without a reconnect (a re-plugged hub on the same node).
- **Test it hardware-free** with the pty fakes: `set_muted(true)` (hung MCU, healthy fd) and
  `sever_link()` (master closed, reads/writes EIO) — `tests/fake_serial_streamer.h` for any
  streaming device, `fake_gemini_pdh.h` for the polled one. Assert: fault latches within the
  threshold, `Connected` still true, static metadata OK, nothing on the wire while faulted,
  and the next frame restores service.

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

### Serial auto-detect scan (`util/serial_by_id_scan.h`)

Every auto-detect `enumerate_*_ports()` scans `/dev/serial/by-id` and, for most
vendors, falls back to raw `/dev/ttyUSBn`/`ttyACMn` nodes when a device isn't
covered by (or visible in) `by-id`. Use the shared helpers below instead of
hand-rolling this — issue #179 found nine near-identical copies of the scan, each
independently broken the same way, before it was centralised:

- **`alpacacore::util::list_serial_by_id(dir)`** instead of a raw
  `std::filesystem::directory_iterator` + `is_symlink()` loop. Built entirely on
  the `std::error_code` overloads, so it never throws `filesystem_error` — a
  device unplugged mid-scan just ends the scan with whatever was already
  collected, instead of aborting the whole `enumerate_*_ports()` call and
  discarding results already found. Still resolve each returned `.path` with
  `std::filesystem::canonical(path, ec)` yourself using the `error_code`
  overload (never the throwing one) — the by-id symlink can go stale between
  the scan and that call too.
- **`alpacacore::util::read_raw_tty_usb_descriptor(port_path)`** +
  **`usb_tty_descriptor_matches(descriptor, {...})`** to filter a raw
  `/dev/ttyUSBn` fallback by USB vendor/manufacturer/product, read straight
  from sysfs — the same fields udev uses to build by-id names. Required
  whenever the raw fallback can run unconditionally (see next point): without
  it, the fallback opens — and for CH340/CH341-class hardware, DTR-resets —
  every serial device on the box, not just this vendor's. Beware the udev
  spelling trap (issue #181): by-id *names* carry udev's underscore mangling
  (`USB_Serial`), but the raw sysfs strings keep their spaces (`USB Serial`) —
  the helper matches patterns against both spellings, so udev-style patterns
  copied from a by-id name filter are fine, but don't "simplify" that
  double-match away.
- **`alpacacore::util::path_exists(path)`** instead of bare
  `std::filesystem::exists(path)` anywhere in an `enumerate_*` scan. The plain
  overload throws `filesystem_error` on a traversal error (e.g. EACCES on a
  parent directory), aborting the whole auto-detect including fallback passes
  (issue #181 — every wrapper had this, and in ZWO it silently replaced a
  `try`/`catch` that existed specifically to protect the WiFi fallback).
- **Run the raw fallback unconditionally, not only when `by-id` is entirely
  absent, and dedupe by resolved canonical path.** Generic USB-serial adapters
  (CH340/CH341, Prolific, FTDI, CP210x) report identical descriptor strings
  with no per-device serial number, so when two of the same chip are plugged
  in at once, udev's by-id naming collides and only ONE gets a symlink — the
  other silently vanishes from a `by-id`-only scan even though the directory
  itself exists. Track already-probed canonical paths in a
  `std::set<std::string>` (or `unordered_set`) populated as the `by-id` pass
  resolves each candidate, and skip any raw-fallback port already in that set
  — otherwise a port tried via `by-id` gets opened (and reset) a second time.
- **Collect candidates first, then probe them concurrently (issue #218).** A
  probe against a port that isn't this device (the iOptron mount shares the
  Prolific chip class with the iEAF and iEFW; a mount controller often shares
  CH340 with the Gemini gear) costs the full handshake timeout — ~5 s for the
  iOptron probes, ~6 s for Gemini — so probing inside the scan loop made
  auto-detect connect time grow linearly with adapter count. Both scan passes
  now only append `{path, name}` to a `candidates` vector; a `std::thread` per
  candidate then runs `probe_port()` into an index-matched results vector, and
  the found ports are emitted in candidate order (so the auto-detect device
  index stays deterministic). Wall time is bounded by one port's worst case.
  Don't move the probe back into the scan loop when copying this pattern to a
  new vendor, and keep the by-id/raw dedupe set — it is what stops two threads
  opening the same port at once.

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
- **Value types must match the ASCOM member type**: ConformU coerces each DeviceState
  value to the member's declared type and treats a failure as "property not included"
  (INFO, not an issue, so it hides in a passing log). `PercentCompleted` is a `short`; the
  camera base emitted it as a double (`0.0`) for a year and every camera log carried the
  INFO line. Emit integers for integer members (`CameraState`, `PercentCompleted`,
  `Position`, …) and doubles only for double members.
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

1. **Router registration** — add vendor/device case to `Router::register_device_from_config` in `AlpacaHTTP/src/http/router.cpp`. This is the dispatch that creates driver instances from persisted JSON config. **Read config fields through `config_has()` / `config_get()`, never `config.contains()` / `config.value()`** (#388): `contains()` is true for an explicit JSON `null` and `value()` THROWS `type_error` rather than returning the default when the stored value is not convertible, so `{"siteLatitude": null}` — trivial to produce from a client that serialises an unset value instead of omitting the key — escaped the vendor branch and came back as an nlohmann type complaint instead of the specific message the field has. The helpers treat null as absence and report a genuinely wrong type as an `AlpacaException` naming the field. **Scope**: this is done for the TOP-LEVEL config fields. The nested `ports[]` sub-objects on the iMate PowerBox / ASIAIR / StellaVita switch branches still use `p.contains(...)` / `p.value(...)`, so `{"ports":[{"pwm": null}]}` still surfaces a raw `type_error` -- the same #388 shape one level down. Use the helpers for new fields at either level; converting the existing `ports[]` readers is tracked separately.
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
- **The router must never call `get_connected()` while `get_connecting()` is
  true — the connect side of the rule above** (SynScan hand controller,
  2026-09, issue #130). The telescope drivers named in
  `async_connectable.h`'s blocking list -- **that comment is the list; this
  paragraph deliberately does not repeat it, because a second copy is what
  went stale for SynScan** -- answer `get_connected()` under the state mutex that their
  `set_connected(true)` holds for the entire handshake, so a
  `get_connected()` call from the `PUT connected` wait or from a `GET
  connected` blocked for the whole connect and the wait's 8 s deadline never
  fired (25 s on a silent handset: five 5 s query timeouts). The
  wrapper-backed switch drivers can block too — their `is_open()` waits for the
  wrapper mutex, which `open()` holds throughout and `close()` holds for its
  two locked phases (it unlocks to join the PWM workers) — but that work is
  all local, so the window is microseconds to milliseconds rather than a
  multi-second serial handshake. Do not describe it more precisely than that
  in prose: the mechanism has been restated wrongly three times, and the bound
  is what the rule depends on. The rule applies to both; only the mutex-holding
  telescopes make it urgent. Every router
  site now reads `get_connecting()` first and short-circuits; while a task
  is in flight `Connected` reports false. A connect request that arrives
  mid-task is still passed to `device->connect()` so `AsyncConnectable` can
  queue it against an in-flight disconnect or drop it against an in-flight
  connect. Driver side, prefer an atomic `connected_` with a lock-free
  getter (every driver does except the ones `async_connectable.h` names --
  SynScan is among those that DO have a lock-free getter, since the #130 fix) —
  the telescopes it lists still take the mutex and rely on the router rule, and the
  wrapper-backed switch drivers it lists lock inside the wrapper's `is_open()` but
  release it before
  `pending_mutex_`, so they rely on the rule without creating the ABBA hazard.
  **`async_connectable.h`'s comment is the single source for both lists, it is
  gated, and no count is stated anywhere** (issue #381):
  `scripts/check_docs_drift.py` classifies every `get_connected()` override
  under `AlpacaCore/src/vendors/` by its body and fails if a blocking one is
  missing from `async_connectable.h`'s list, if a lock-free one is still named
  there, or if one cannot be classified at all. Bare numbers used to be
  restated in four places, had nothing tying them to the code, and drifted
  repeatedly — including a fresh stale count introduced by the PR that was
  correcting the others. State the rule, not the arithmetic. Regression tests:
  `AlpacaHTTP/tests/test_routing.cpp` (mutex-holding slow stub) and
  `AlpacaCore/tests/test_synscan_async_park.cpp`.
  **Known trade-off:** while a task is in flight, `Connected` reports false
  for every client, including one whose `PUT connected` reply already came
  back at the 8 s deadline with the connect still proceeding — a Platform 6
  client that treats that combination as a hard failure gives up on a
  connect that may still succeed moments later. Accepted because the
  alternative (reading `get_connected()` directly) is the phantom-link bug
  this rule fixes; there is no per-driver signal yet for which
  `get_connected()` implementations are safe to read mid-task (the lock-free
  majority) versus which aren't (the telescopes above and the
  wrapper-backed switches).
  **Known gap (narrow, code review on PR #3):** `get_connecting()` and
  `get_connected()` are two separate calls, not one atomic snapshot — if a
  connect task starts in the gap between them, the `get_connected()` call
  can still block on a mutex-holding driver's handshake for the telescopes above and the
  wrapper-backed switches (their wrapper `open()` holds the same mutex `is_open()` takes).
  Far narrower than the bug this rule fixes (needs a second request to land
  in a specific few-instruction window, not just a slow connect), and not
  worth a structural fix here: closing it means every driver exposing one
  atomic "get state" call instead of two, a bigger change than this PR's
  scope. Left as a known risk rather than solved.
- **`Connected` is per-client, refcounted in the router — never wire an
  endpoint straight to `device->connect()`/`disconnect()`** (issue #160).
  Alpaca is designed for several clients sharing one device (imaging app +
  guider on the same mount), so the router keeps a per-device registry of
  connected clients keyed by a length-prefixed `<addrlen>#<addr>#<ClientID>`
  composite (issue #163: the server stamps `Request::remote_address()` from
  `getpeername`, so ClientID-less clients on different hosts get distinct
  anonymous slots; the length prefix keeps a client-supplied ClientID from
  forging a collision with another (address, ClientID) pair, and an empty
  address degrades to ClientID-only) via `Router::register_client_connection` and
  friends: first client in powers the upstream link, `PUT connected=false`
  (and Platform 7 `disconnect`) only tears it down when the LAST registered
  client leaves, and `GET connected` answers the *caller's* registration
  AND-ed with device state (ClientID-less requests read raw device state on
  GET). Supporting rules: a dead upstream link
  (`!get_connected() && !get_connecting()`) clears the whole registry so
  every client observes the failure; a failed connect drops the caller's
  registration; any request from a client refreshes its registration, and
  registrations idle >10 min expire so vanished clients can't pin the
  device connected; device removal clears the registry entry (the map is
  keyed by driver pointer — a later driver at a recycled address must not
  inherit registrations). A per-device connection-op mutex
  (`device_connection_op_mutex`) serializes the whole decision + driver call
  so a client connecting during another client's last-out teardown can't
  register against a link about to drop; it is held across the blocking
  connect/disconnect waits (same-device ops queue) while the registry mutex
  itself still never spans driver calls — and `clear_client_connections`
  must never erase the op-mutex entry (it runs under the op mutex; erasing
  would let a concurrent op mint a fresh mutex and bypass serialization) —
  op-mutex entries are reaped only by `purge_device_connection_state`,
  called in `handle_remove_device` AFTER the op lock is released and the
  device is out of the DeviceRegistry (issue #162); `device_is_current`
  guards every map insertion so a straggler request that fetched the device
  shared_ptr before a removedevice can't re-insert entries nobody will reap
  (registering handlers throw InvalidOperation, the op-mutex accessor hands
  back an ephemeral mutex). If you add any new
  endpoint that connects or disconnects a device, route the decision
  through this registry and take the op mutex.
- **Connections are persistent (HTTP keep-alive) — never emit
  `Connection: close` on a normal response** (2026-09-07). `Response::to_string()`
  defaulted to `Connection: close`, and `Server::handle_connection` served one
  request per TCP connection — real non-compliance with the README's existing
  keep-alive claim, and unnecessary overhead for every long-lived Alpaca
  client (NINA, PHD2, ConformU). Connections now persist across requests
  (`Server::serve_one_request` serves one; the reactor, below, holds the
  connection between them) (RFC 7230 §6.3: HTTP/1.1 persists unless the client sends
  `Connection: close`, HTTP/1.0 closes unless it sends `Connection: keep-alive`),
  carries pipelined surplus bytes into the next `read_request`, marks the
  response `Connection: keep-alive`, respects a handler-set `Connection`
  header, and drops an idle connection after `kKeepAliveIdleSeconds` (15 s,
  well under the per-request slowloris bound) so idle clients cannot pin the
  worker pool. Error responses (`send_error`) still close. Regression tests:
  the keep-alive cases in `AlpacaHTTP/tests/test_server_socket.cpp`.
  **Not a fix for ConformU FAST-target misses** (Raspberry Pi 3B, ZWO
  ASI533MC Pro and Sky-Watcher EQM-35): three properties per device
  (`CameraState`/`CameraXSize`/`SensorType`; `DeviceState`/`AlignmentMode`/
  `EquatorialSystem`) deterministically miss the 0.1 s FAST target by
  ~150-200 ms across every run, while the server itself answers in 2-8 ms and
  ~50 other FAST members on the same run are within 20 ms — so it looked like
  a transport cost, and an initial `strace` on both processes (server, and
  ConformU under strace) showed the gap followed by a `socket()`/`connect()`
  pair, which read as ConformU stalling before opening its next connection.
  That reading was wrong: re-run after this fix, with `strace` confirming a
  single `accept()` for the entire 274-request run (one TCP connection, real
  keep-alive), reproduced the identical three misses at the identical
  magnitudes. Correlated tracing during a live miss showed the server idle in
  `recvfrom` the whole gap while ConformU's only activity was
  `futex`/`epoll_pwait` — a stall entirely inside ConformU's own .NET process,
  unrelated to sockets. Diagnostic rule this earns: a FAST miss on a constant
  getter that curl answers in ~2 ms is not driver latency, but don't assume
  transport either — the `socket()`/`connect()` adjacency in the first trace
  was coincidental, not causal; correlate both processes on one clock and
  confirm before writing up the mechanism. **Resolved 2026-09-12**: the stall
  was ConformU 4.5.0's own arm64 release bug, not AlpacaBridge or a Pi 3B
  hardware limit. The official `linux-arm64.tar.xz` 4.5.0 asset ships without
  `PublishReadyToRun`, so .NET JIT-compiles each generic-over-value-type
  instantiation on first use, charging the first member of each response
  type ~130-220 ms regardless of how fast the driver answers
  ([ConformU#31](https://github.com/ASCOMInitiative/ConformU/issues/31),
  fixed in 4.5.1; see `SUPPORTED-DRIVERS.md`'s General Notes). Confirmed on
  this exact rig, same driver build, only ConformU swapped for 4.5.1 (PR
  #462): `CameraState` 0.187s→0.015s, `CameraXSize` 0.168s→0.005s,
  `SensorType` 0.172s→0.004s. The 4.5.1 side is the committed report at
  `AlpacaCore/conformu/ZWO/ASI/ASI533MC Pro/Linux-arm64.txt`; the 4.5.0
  before-numbers exist only in PR #462's discussion. The mount's identical three-member signature is
  presumed the same cause, not independently re-confirmed on a Pi 3B. No
  Pi 5 needed — install 4.5.1 and re-run.
- **Persistent connections are capped at `kMaxRequestsPerConnection` (1000
  requests)** (2026-09-08). Making connections persistent removed the
  per-request handshake cost, but also removed the only thing that used to
  free a worker automatically: with a fixed 32-thread pool and no
  backpressure on `connection_queue_`, a handful of clients that simply keep
  a connection alive (sending a request at least every `kKeepAliveIdleSeconds`)
  — accidentally, from several long-lived Alpaca clients, or adversarially —
  could each pin one worker indefinitely. `handle_connection` now forces
  `keep_alive = false` once a connection has served this many requests,
  which cannot be overridden back to keep-alive by the client or a handler's
  own `Connection` header. The reconnect this costs a well-behaved long-lived
  client (PHD2 autoguiding, ConformU) is negligible next to the per-request
  handshake this whole feature exists to avoid. **Also capped by wall clock**:
  `Config::keep_alive_lifetime_seconds` (300 s default; settable so the cap
  can be tested, see the lifetime-cap case in `test_server_socket.cpp`)
  forces the same reconnect regardless of request count, since the count cap
  alone still lets a connection that sends one request every
  `kKeepAliveIdleSeconds` stay persistent for up to ~4 hours (1000 × 15s)
  and simply reconnect afterward (PR #2 review, round 2). **The lifetime cap
  is enforced only on a response** (`Connection: close` on the first one
  past it), never by closing an idle socket the moment the cap passes: that
  would race a polling client's next request, which would meet EOF instead
  of an answer, and .NET `HttpClient` does not retry a PUT on a dead pooled
  connection. An idle connection past the cap just runs out its idle gap.
  The handler-set `Connection` header comparison (`server.cpp`) is
  case-insensitive for the same reason `wants_keep_alive` is on the request
  side. The outgoing
  `Connection` header is now always rewritten to match the final `keep_alive`
  decision, rather than only set when absent -- a handler that had set
  `Connection: keep-alive` before the count/lifetime caps forced closure
  would otherwise leave that stale header on the wire, telling the client
  keep-alive while the server closes right after (review round 3). Not
  reachable via any handler today, fixed defensively.
- **`kMaxRequestsPerConnection` hardware-validated** (2026-09-09, EQM-35 rig
  Pi 3B, `astropi`): a standalone build of this branch was run on a spare
  port (6900, discovery off, no vendor devices attached — the live
  `alpacabridge.service` on 6800 and the mount's serial port were untouched
  throughout) and driven with a script sending 1000 requests down one TCP
  connection. Requests 1-999 each answered `Connection: keep-alive`; request
  1000 answered `Connection: close` and the server actually closed the
  socket (confirmed via a follow-up `recv` returning EOF, not just the
  header). All 1000 requests completed in 0.33s with no dropped or stuck
  connection, and a fresh reconnect immediately after got `keep-alive`
  again, confirming the server isn't left in a bad state post-cap. Only
  the x86 loopback unit test (`test_server_socket.cpp`) had exercised this
  before; this is the first real-network, real-hardware confirmation the
  count-based cap actually fires.
- **No ConformU run**, deliberately: this change touches only `AlpacaHTTP`'s
  connection-handling layer, not any device driver, so there is no new
  device behavior to conformance-check.
- **The keep-alive loop checks `running_` and closes on the next response
  once `stop()` has begun** (2026-09-09, final review pass). `stop()` joins
  every worker, and a worker only leaves `handle_connection`'s loop when the
  connection ends — so an ACTIVE client (NINA/PHD2 polling every second)
  held its worker, and therefore `stop()`, until the 300s lifetime cap.
  systemd's default 90s `TimeoutStopSec` would SIGKILL the service first,
  and the same applies to the management restart/shutdown endpoints, which
  go through `stop()` on the main thread. Before keep-alive a worker only
  ever held one request, so this was a genuine regression the caps did not
  cover: they bound how long a connection may live, not whether it outlives
  the server. Measured with a client sending every 2s: `stop()` blocked
  26,006 ms and served 13 further requests before the check, 1 ms after.
  With the reactor (below) no worker is ever parked, so `stop()` no longer
  waits out an idle gap at all: a request in flight is answered with
  `Connection: close`, a request already on the wire at the reactor's final
  zero-timeout poll is handed to the draining workers and answered the same
  way, and idle connections are all sent FIN at once, given one shared
  100 ms window, drained and closed (measured: 114 ms with three parked
  clients; 14 s on the pre-reactor design with two). Regression test:
  the last case in `AlpacaHTTP/tests/test_server_socket.cpp` (it has to be
  last; it stops the server).
- **`Response` header names compare case-insensitively** (2026-09-09, review
  round 5). `Response::headers_` was a plain case-sensitive map while
  `Request` lowercases its keys on parse, so the keep-alive override's
  `get_header("Connection")` / `set_header("Connection", ...)` would have
  missed a handler's `connection: keep-alive` and emitted BOTH lines — the
  stale-keep-alive-on-a-closing-socket bug (round 3) back through a different
  door. `set_header` now replaces any other spelling of the field (keeping
  the caller's casing for the wire), `get_header` and `to_string()`'s
  default-`close` check match case-insensitively. No handler sets a
  `Connection` header today; the override exists precisely for the day one
  does. Test: the `Response` case at the end of `test_routing.cpp`.
- **A connection with carried (pipelined) bytes is never parked** (issue
  #234). The reactor polls the *socket*, so bytes already read into
  `Connection::carried` would be invisible to it; the worker keeps serving
  until `carried` is empty (after stripping a lone trailing CRLF, which is
  padding, not a request). This also settles the per-request timeout
  question that two review rounds on #233 got wrong in different ways: a
  worker only ever reads a connection whose request has already begun
  arriving, so every recv runs under the plain 30 s `kSocketTimeoutSeconds`
  bound set at accept time and there is no idle timeout to restore. Tests:
  the slow-body and pre-carried-headers cases in `test_server_socket.cpp`
  (16 s gap inside request 2's body, in its own write and pre-carried).
- **Persistence is opt-in: a connection may only stay open for an exchange we
  framed correctly** (2026-09-08, PR #233 review). Keep-alive turned every
  latent framing gap into a stream desync, because leftover or mis-framed
  bytes are now read as the *next* request instead of dying with the
  connection. `may_persist(request, response)` in `server.cpp` is the single
  gate: an unknown method or a response without `Content-Length` is answered
  normally and then closed. Do not add a per-bug patch for each new framing
  construct — widen the gate instead, so the failure mode of anything we do
  not understand is one extra TCP handshake rather than a client reading our
  bytes as the head of its next response. In particular **do not add `HEAD` to
  `Request::parse_method`**: it would route, and the router answers with a
  body that a HEAD client must not receive. Tests: the HEAD and chunked cases
  in `test_server_socket.cpp`, and the `Content-Length` default case in
  `test_routing.cpp` (every `Response::to_string()` emits exactly one).
- **`Transfer-Encoding` is rejected with 501, not ignored** (same review).
  `read_request` frames bodies from `Content-Length` only, so a chunked body
  read as zero-length left its chunk framing on the wire to be parsed as the
  next request — request-smuggling-shaped behind any intermediary that does
  understand chunked. If chunked support is ever added, it must be added to
  the body reader *and* the gate above, together. Test: the chunked case in
  `test_server_socket.cpp`.
- **Close the write side and drain before `close()`** (same review). On Linux
  `close()` on a socket with unread bytes queued sends RST, and the peer's
  stack then discards its receive buffer — including a response we sent that
  it has not read. Keep-alive makes this ordinary: at the request-count and
  lifetime caps and on the `stop()`/restart path, a polling client usually has
  its next request already in flight. Use `util::socket_close_graceful`, never
  a bare `util::socket_close`, on a **client** socket. The drain budget is
  deliberately short (100 ms x 4) so it cannot become the worker-pinning
  problem it sits next to. `Server::close_connection` is the single
  client-close site (it also keeps `live_connections_` honest); the one
  non-graceful call, from the reactor at `stop()`, is safe because a socket
  that was not readable at the last poll has nothing queued, so `close()`
  sends FIN. The listener closed by `stop()` is not a client connection. Test: the
  graceful-close case in `test_server_socket.cpp` (a `Connection: close`
  request with 10 KB of trailing bytes the server never reads; on Linux a
  bare `close()` still delivered the queued response but ended the
  connection in `ECONNRESET` instead of EOF, and Windows stacks discard the
  queued response outright).
- **Idle connections live on the reactor, never on a worker; `thread_pool_size`
  bounds concurrent REQUESTS** (2026-09-08, issue #234, replaces the #233
  reserve stopgap). `Server::reactor_loop` (one thread) parks every idle
  connection on a `poll()` set with a self-pipe for wakeups; when a
  connection becomes readable it goes to `ready_queue_`, a worker serves
  exactly one request (`serve_one_request`), keeps going only while it holds
  carried bytes, then hands the connection back (`park_connection`). A
  `Connection` (fd, remote address, carried bytes, request count, open time,
  deadline) has exactly one owner at a time and moves by `unique_ptr`. Rules
  this earns:
  - Never block in the reactor. Expired connections are handed to a worker
    marked `close_only` so the graceful drain happens off the poll thread,
    and the hardware-RTC probe (#314), which can sit on a wedged I2C bus for
    about a second, runs on its own low-frequency timer thread rather than
    between two `poll()` calls.
    The one exception is the final pass at `stop()`: after a zero-timeout
    poll hands already-arrived requests to the draining workers, every
    remaining idle socket gets `shutdown(SHUT_WR)`, one shared 100 ms
    `poll()` so peers can react to the FIN and in-flight bytes can land,
    then a non-blocking drain and `close()`. That is `socket_close_graceful`
    applied to all of them in parallel; a plain `close()` straight after the
    zero-timeout poll left a microsecond window for an RST (PR #235 review).
  - Workers carry a generation number (`worker_generation_`, bumped by every
    `run_server()`, waits notified). A worker that detached itself because
    `stop()` was called on it (no current handler does; the management
    endpoints restart on a detached thread) exits on the generation check
    instead of surviving as an extra thread once `start()` clears
    `shutdown_workers_`. Wake permits are released per live worker
    (`worker_count_`), not per `thread_pool_size`, so any number of detached
    stale workers get their wake-and-exit. **Count at spawn, not in the
    thread body**: a `stop()` landing before a new thread executes its first
    instruction would otherwise undercount, leave that thread with no permit,
    and hang the join (PR #235 review round 4). `run_server()`'s spawn phase
    and `stop()`'s reactor/worker teardown are serialized by
    `lifecycle_mutex_`; `stop()` releases it before joining the server
    thread (which may be about to take it), and the spawn phase bails out
    under it when `running_` is already false, so `start_async()` followed
    at once by `stop()` is safe. Test: the churn case in
    `test_server_socket.cpp` (20 start/stop pairs with no settle time, then
    a served request). The reactor keeps a self-detach branch too, but it runs
    no handler code and cannot be the caller.
  - The reactor's wake pipe is created once in the constructor and closed
    only in the destructor. It is read lock-free by `wake_reactor()` from
    any thread, and a worker orphaned across a restart could still call
    that while a per-start recreation was in flight (PR #235 review round
    3); immutable descriptors have no such race.
  - **No server thread is ever detached.** A thread `stop()` cannot join
    because it is running on it (a handler calling `stop()` synchronously;
    no current handler does) goes into `orphaned_threads_`, and the next
    `stop()` from another thread or the destructor joins it. The threads this
    covers are the accept/server thread, the reactor, the worker pool and the
    RTC probe timer (`rtc_probe_thread_`, #314) -- the last spawns and joins
    alongside the reactor and takes no lock `stop()` holds. So nothing
    can touch a `Server`'s members, the wake pipe included, after the
    destructor returns (review round 5). Destroying a `Server` from inside
    one of its own handlers is not supported.
  - The reactor enforces only the idle gap (`kKeepAliveIdleSeconds`) and the
    first-request slowloris bound (`kSocketTimeoutSeconds`, so a client that
    connects and never sends costs no worker). Caps that should end with a
    `Connection: close` response (request count, lifetime) belong in
    `serve_one_request`, see the lifetime note above.
  - `Config::max_connections` (512 default; `RLIMIT_NOFILE` is 1024 on a
    typical systemd unit and the other half is for SDKs, serial ports and
    logs) bounds live connections across all owners. Both it and
    `keep_alive_lifetime_seconds` are settable from the config file
    (`http:` section keys of the same name) and the environment
    (`ALPACAHTTP_MAX_CONNECTIONS`, `ALPACAHTTP_KEEP_ALIVE_LIFETIME_SECONDS`),
    routed through the clamping setters so every path clamps alike; tested
    in `test_config.cpp`. At the bound the accept
    loop pauses and new clients wait in the listen backlog (64) rather than
    being refused; an idle connection expires within 15 s.
  - Do not reintroduce a worker-side counter or reserve: the previous design
    counted busy workers as parked and pushed clients to close-per-request at
    exactly the busiest moments (review of #233).
  - `live_connections_` is exact by construction (one increment at accept,
    one decrement in `close_connection`) and is **never reset**; a
    connection that straddles a management restart balances in whichever
    generation it closes. `reset_queues_for_start()` closes anything left in
    `ready_queue_`/`reactor_incoming_` through `close_connection` rather than
    clearing it. (PR #235 review flagged a start-time reset as an underflow
    that would gate `accept()` forever; the restart endpoint runs `stop()` on
    the router's detached thread, so every connection is already closed and
    the count already zero before the reset ran, but the reset was wrong on
    principle and is gone.)
  - Restart-path tests must not poll `is_running()` right after the restart
    response: the router fires the callback 100 ms later on a detached
    thread, so a true seen before `stop()` begins is the OLD generation, and
    a client connecting then lands in a listener about to close and reads a
    reset. Wait for a parked bystander to see EOF (proof `stop()` ran), then
    for `is_running()`, then retry `connect()` (it goes true before the new
    listener is bound). And lines "missing" from a test log after an
    `EXPECT` abort are usually buffered stdout lost at `abort()`, not a hang;
    confirm with a backtrace (`pidof test_server_socket`, never `pgrep -f`
    with a pattern that matches your own shell) before chasing one.
  Tests in `test_server_socket.cpp`: the reactor pool case (2 workers, 4
  idle keep-alive connections all kept alive and all served again, 3
  connect-and-never-send clients, a late client still served), the
  lifetime-cap case (2 s cap, own server), the max-connections case (bound
  of 2, third client waits in the backlog and is served once one is
  released), the restart case (two management restarts back to back on a
  bounded server, each with a parked bystander closed and three fresh
  clients served afterwards), and the idle-parked assertions in the final
  `stop()` case. On
  the pre-reactor design the pool case fails at its second keep-alive
  assertion and the `stop()` case fails on a 14 s stop.
- **Test-suite hygiene for socket tests** (same review). `peer_closed()` must
  save and restore `SO_RCVTIMEO` — leaving its short budget on the socket made
  every later `read_one_response()` flaky on loaded CI and reported a slow
  response as "server closed". The test `send_all` must pass `MSG_NOSIGNAL`,
  since several cases deliberately provoke a server-side close and the suite
  installs no SIGPIPE handler.
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

Every new driver **must** ship with at least the following 8 unit test cases, plus the config round-trip test (case 9). Use the existing tests (e.g. `test_svbony_camera.cpp`, `test_gemini_focuser.cpp`) as reference.

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

6. **Value range validation** — invalid inputs must throw `AlpacaException` with `error_code() == AlpacaError::InvalidValue`, not silently normalize or throw a generic error (ConformU specifically tests boundary values).

7. **State machine contracts** — device state follows ASCOM rules without needing hardware (e.g. `CameraState == Idle` before any exposure, `Slewing == false` when not connected, `IsPulseGuiding == false` when idle). These have caught real bugs: iOptron's settle loop prematurely declared slews complete, SynScan's `IsPulseGuiding` always returned false, SVBONY's `CameraState` got stuck after SDK hangs.

8. **Unsupported method error codes** — a method the device doesn't support must throw with the correct error code (usually `InvalidOperation` or `MethodNotImplemented`), not a generic `DriverException`. ConformU distinguishes "not implemented" from "driver error."

   Cases 6-8 are the **ASCOM contract tests**: they exist specifically because a generic "does it throw?" test (case 3/4) is not enough to pass ConformU, which checks the exact Alpaca error code and state-machine behavior. See `/driver-build` Step 7 for the full pattern, worked examples per device type, and the `require_alpaca_error` helper. **Minimum 8 test cases, 30+ assertions total** — cases 6-8 alone should add 10-15 assertions on top of the 5 basic cases; if you have significantly fewer you are probably not testing enough error codes and state transitions.

9. **Config save→load round-trip** in `AlpacaHTTP/tests/test_routing.cpp` — `configuredevice` then read back `configureddevices` and assert **every persisted field survives** (index/id, filter names, PWM/port config, etc.). The automated catch for the two silent-data-loss classes described in [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors). Model it on the existing ToupTek AFW filter-wheel round-trip test. This is an `AlpacaHTTP`-level integration test, additional to the 8 vendor unit tests above, not a substitute for cases 6-8.

### Hardware-free driver tests via the SDK seam (ToupTek and QHY — extend to other vendors)

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
  the reusable pattern from issue #104. The two existing seams differ in one
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
  exposure, temperature and cooler-off workers join with a bounded timeout and
  *detach* on expiry, and its pulse-guide worker is detached by design, so a
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
`AlpacaCore/tests/fake_pty_write.h`, passing its own `stop_` flag. A bare
`write(master_fd_, ...)` on a blocking master parks the fake's worker thread as
soon as the driver stops draining — which is normal as a concurrency test winds
down — and the destructor's `join()` then never returns, because the thread is
asleep in `write()` and never reaches the `stop_` check. The result is a hung
process, not a failing test, and it is a race, so it shows up as an occasional
CI hang rather than a reproducible red (#424, the shape #364 describes).

Dropping a reply is the correct answer here: a reply the driver is not draining
is one it was never going to read, and a fake whose destructor can hang is worse
than one that drops a frame.

### Test CMake Integration

When adding a test file for a new vendor device:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list in `AlpacaCore/tests/CMakeLists.txt`, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.
- Build and run all tests (`cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests`) before considering the driver complete.

## Continuous Integration and Pre-flight

- CI (`.github/workflows/ci.yml`) runs on every PR, all on the native arm64 runner: `build-test` (vendors OFF) + `build-vendors` (vendors ON), `sanitizers` (ASan+UBSan), `sanitizers-tsan` (ThreadSanitizer over the `[stress]` connect/disconnect/operate concurrency suite, all vendors ON, plus `[stress-guard]` for the harness's own self-tests), `clang-format`, `clang-tidy`, `cppcheck`, `unicode`, `shellcheck`, `javascript`, and `zizmor`.
- **Every job in every workflow carries a `timeout-minutes` bound** (issue #363). The numbers are sized from the observed healthy runtime of recent green runs with wide headroom (`sanitizers-tsan` 30 min against a healthy max of 5, `build-vendors`/`sanitizers` 25, `build-test` 20, `clang-tidy` 25 -- it does the all-vendors build `build-vendors` does plus libgpiod from source and `clang-tidy-diff` over every changed line, so it gets that job's bound rather than a smaller one -- `cppcheck` 20 -- its dominant cost is building cppcheck 2.17 from source on the runner with no cache, so it gets the same bound as the build jobs rather than a text-scan-sized one -- the text scans 10; `release` 10, `codeql` 30, `claude-review` 45). They exist because the `[stress]` suite is the one place a regression can *hang* rather than fail, and GitHub's 6-hour default turned that into six hours of runner time before any signal. **When you add a job, give it a bound**, and when a job legitimately outgrows its bound raise the number rather than trimming the work to fit -- these are a backstop against a wedge, not a performance target. Note the deliberate gap on `claude-review`: its 45 min bound is longer than `/pr-checker`'s 30 min verdict-poll budget, so a review running past 30 min times the skill out while CI still lets the job finish. That is the intended precedence (the bound exists to catch a wedged job, not to pace the reviewer); a poll timeout is a re-poll, not a broken workflow.
- **Run `scripts/ci_preflight.sh` before opening a PR** (it is the `/submit-pr` Step 4 hard gate). It reproduces the CI gates locally, auto-installing missing tools, and exits non-zero if any mandatory gate fails — catching failures before they ever reach CI.
- **cppcheck is pinned to 2.17.x, built from source in CI.** The `ubuntu-24.04-arm` runner's apt cppcheck is 2.13, which classifies some checks differently from the 2.17 on a Debian Trixie dev box (e.g. `virtualCallInConstructor` is a `warning` in 2.13 but reclassified in 2.17). Since `ci_preflight.sh` runs whatever cppcheck the dev box has, that version skew let the local pre-flight and CI disagree. Building 2.17 from source (checksum-verified, mirroring the libgpiod-from-source step) keeps them aligned. **Keep the cppcheck `--suppress` list identical between `ci.yml` and `ci_preflight.sh`** — `scripts/check_docs_drift.py` (the `docs-drift` CI job / pre-flight gate) now fails if they diverge, so this can't silently drift again.
- **Web UI JavaScript is gated by `node --check` AND `node --test`** (the `javascript` job + pre-flight gate). The web UI is hand-written static JS served as-is, with no bundler and no `package.json`, so the syntax check was the only gate for a long time — and a syntax check catches a missing brace and nothing else. That was defensible while `app.js` was DOM wiring; it stopped being defensible when the file grew pure functions with a real contract (issue #385). **Pure helpers go in `AlpacaHTTP/web/format.js`, not `app.js`**: that file touches no DOM and no `app.js` global, `index.html` loads it first, and it ends with a `typeof module !== 'undefined'` export block that browsers ignore, so `AlpacaHTTP/tests/web/*.test.js` can `require` it from Node with no browser stub. `node --test` ships with the Node CI already installs, so there is nothing to add to the toolchain. A helper that reaches for `document` belongs in `app.js` and stays untested — keep the split honest rather than growing a DOM stub. The first cases are the by-hand verification table from PR #359 made executable (several `TZ` settings, local midnight, and `Intl.DateTimeFormat` patched to throw, to return incomplete parts, and to answer in 12-hour form); each was mutation-verified against the guard it covers, and the 12-hour case is the one that matters most, because it renders a plausible-looking WRONG time rather than an obvious failure.
- `zizmor`'s pinned version + sha256 appear in both `ci.yml` and `ci_preflight.sh` — bump them together; `scripts/check_docs_drift.py` fails the build if they disagree. The same script also fails if a `docs/development.md` build-options table row goes missing for a CMake `ALPACACORE_ENABLE_*` option, if `VERSION` and the README badge disagree, if AGENTS.md references a repo path that doesn't exist, if the QHYSDK seam's interface / `LockedQHYSDK` / sweep lists disagree (issue #394), if the `sanitizers-tsan` job's filtered runs or their zero-test greps differ from the ones `ci_preflight.sh` spells out (issue #341), or if `async_connectable.h`'s blocking-`get_connected()` lists drift from the drivers (issue #381): it classifies every `get_connected()` override under `AlpacaCore/src/vendors/` by its body and fails on a blocking driver missing from a list, a lock-free one still named in it, or a count of either list stated anywhere the globs reach.
- **Concurrency now has automated coverage — but only where a driver is registered with the stress harness.** The `sanitizers-tsan` job (issue #101) builds all-vendors with ThreadSanitizer and runs the `[stress]` connect/disconnect/operate suite (`AlpacaCore/tests/concurrency_stress.h`): lifecycle storms from N threads, destruction racing an in-flight connect, and the racing-disconnect-never-dropped settle check. Locally: `RUN_TSAN=1 ./scripts/ci_preflight.sh`. Registered so far: ToupTek camera / AFW / AAF focuser / thermal switch (over the fake SDK seam, wrapped in `LockedToupTekSDK`), ZWO EFW + camera + EAF focuser + CAA rotator + dew heater switch, Player One Phoenix + camera + thermal switch, SVBONY camera, Bisque, OnStep, and — over the loopback fake-mount TCP seam (`tests/fake_mount_server.h`, which drives drivers into the *connected* state so the poll/pulse/GOTO/teardown threads actually run) — the ZWO, Celestron, SynScan, and iOptron telescopes, plus the SkyWatcher telescope over its own loopback UDP simulator (`tests/fake_skywatcher_mount.h`), plus (fail-fast, no fake seam) the iOptron iEFW filter wheel, iEAF focuser, and iMate PowerBox Switch, and the Astroasis Oasis focuser (hidapi, no fake seam exists), plus the WandererAstro cover calibrator, filter wheel and box switch over the pty streamer fake (`tests/fake_serial_streamer.h`) with the request/response rotator fail-fast, plus the Gemini PDH Advanced 3 Switch and Flat Panel Pro CoverCalibrator over their pty-backed fakes (`tests/fake_gemini_pdh.h`, `tests/fake_gemini_flatpanel.h`) with the Gemini focuser fail-fast, and the WeeWX ObservingConditions driver over an unreachable URL. **When you add or substantially change a driver, add a `[stress]` TEST_CASE for it** — one factory + one operate callback (see `test_touptek_concurrency_stress.cpp` for the overall shape — but take the per-call guard from this paragraph, not from that file: every merged registration predates `StressCallGuard`, and of the 15, five — astroasis, gemini, playerone, wandererastro and weewx — define a local `call()` helper at all (four as a lambda, wandererastro as a file-scope template). Six more wrap *some* calls in inline `try`/`catch` blocks, and four — touptek included — have no per-call isolation whatsoever, so their operate callbacks stop at the first throw and exercise one call instead of all of them. Migration is tracked in issue #326). Wrap each call in the operate callback with `alpacacore::test::StressCallGuard` (`concurrency_stress.h`, issue #322) rather than a local `try/catch` — `run_lifecycle_stress` wraps the WHOLE callback in one try/catch, not each call inside it, so on a fail-fast path a single throw silently skips every call after it unless each one is guarded individually. `StressCallGuard` samples **one line per distinct failure mode with its occurrence count**, not the first N events (#377) — the old cap let one thread faulting in a tight loop fill every slot with copies of one message before another thread recorded once, which is worst in a *connected* registration where a live driver throws often and the single distinct failure that mattered is the one crowded out. It swallows the expected `NotConnected` by default and counts everything else it catches (a non-`std::exception` throw is never caught by anything here and still `std::terminate`s the binary, exactly as it would without the guard), so the case ends with `INFO(guard.report());` followed by `CHECK(guard.unexpected_count() == 0)` — **both lines, always**: only the `CHECK` turns counting into a failure, and a file that wraps every call correctly and omits it passes whatever the storm throws while *looking* like it follows the pattern (#379) — the `INFO` is what makes a failure legible, since without it the `CHECK` reports only the expansion (`3 == 0`) and names nothing it swallowed — this replaces re-deciding the guard's catch type per file, which flip-flopped across review rounds before #322. Its constructor argument **REPLACES** the default set rather than adding to it — pass `{NotConnected, PropertyNotImplemented}`, not just `{PropertyNotImplemented}`, or every racing-disconnect throw in the storm is counted as a regression and the case fails nondeterministically. **A registration that runs *connected* (over `fake_mount_server.h`, `fake_skywatcher_mount.h`, or any full-seam fake) must widen the expected set explicitly this way** — `NotConnected` alone fits a never-connected, fail-fast path, but an operate callback exercising a live driver can legitimately hit `InvalidValue`, `MethodNotImplemented`, or `InvalidWhileParked` (a slew racing a park) from ordinary calls too — remember `NotImplemented`/`PropertyNotImplemented`/`MethodNotImplemented` all share one numeric code (see the class doc), so they're not separately distinguishable inside the expected set. Drivers without a registration are still covered only by code review against the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver); do not assume green CI means thread-safe for them. **The QHY SDK seam's three parallel lists are gated** (#394): `scripts/check_docs_drift.py` compares `QHYSDK`'s pure virtuals, `LockedQHYSDK`'s overrides and the forward-sweep method list in `test_qhy_fake_sdk.cpp`, and separately fails on a forward that does not go through `locked()`. The compiler forces a forward to EXIST (an unimplemented pure virtual leaves the decorator abstract) but never that it takes the mutex, which is the only reason the decorator exists — an unlocked forward makes the fake racy under a storm and produces a TSan report naming the *fake*, the exact confusion the decorator was built to prevent. Model any future SDK decorator the same way. `scripts/check_stress_registration.py` (the `stress-registration` CI job / pre-flight gate) fails on any vendor/device-type pair that is neither registered nor explicitly allow-listed there, so the currently-unregistered drivers are tracked in one place instead of only in this paragraph. **Name a registration file `<something>_concurrency_stress.cpp` and add it inside the vendor's `if(TARGET alpacacore_<vendor>)` block in `AlpacaCore/tests/CMakeLists.txt`** — the gate enforces both halves: the file glob carries no `test_` prefix (issue #376: the prefix used to be load-bearing, so a file following the documented naming was rejected as a stray `[stress]` case), and a registration file listed in the unconditional `TEST_SOURCES` block is a failure (issue #396), because rule 3 below assumes every `[stress]` case compiles conditionally. That covers a file listed *both* ways too, since CMake de-duplicates the repeated source and the ungated mention is the one that decides, and `if(NOT TARGET ...)` does not count as gating, because it compiles the file exactly when the vendor is absent — one that does not re-satisfies the TSan zero-test grep with no vendor coverage at all, which is the exact failure that grep exists to catch, reached from the other direction. The script's C++ scans strip comments first (issue #386), so an illustrative case macro in a doc comment is documentation and not a registration — `concurrency_stress.h` carries one as the live proof — but **string literals are not stripped**, so keep examples in comments; and they cover every Catch2 registration macro (`SCENARIO`, the `TEMPLATE_*` family, the `_METHOD` fixture variants, `METHOD_AS_TEST_CASE`, `TEST_CASE_PERSISTENT_FIXTURE`, `REGISTER_TEST_CASE`) -- if Catch2 ever grows another, add it to `CATCH_TEST_MACROS`, since a macro missing from that tuple is invisible to the stray-`[stress]` rule and join adjacent tag literals the way the preprocessor does, so `"[vendor][camera]" "[stress]"` is three tags rather than two (issue #393). It separately rejects two tag mistakes per TEST_CASE, each scoped to where that mistake actually costs something: a `[stress-guard]`-without-`[stress]` case inside a `*_concurrency_stress.cpp` file (that tag is for harness self-tests only — a registration wearing it would still get TSan and still read as registered while dropping out of the vendor-coverage count), and a `[stress]` case anywhere else under `AlpacaCore/tests/` (a file outside that glob can compile unconditionally, in which case one such case alone satisfies the TSan job's vendor zero-coverage grep and makes it vacuous — this actually happened, see `test_async_connectable.cpp`). Note the first rule is deliberately limited to registration files: `[stress-guard]` is legitimate anywhere else, which is the whole point of the tag — a vendor case parked outside that glob wearing it would read as registered to a human without counting toward vendor coverage, but that is a naming problem rather than a tag one, and the gate does not try to catch it. Note also that the gate keys on vendor/device-type pairs, so a registered driver masks every other driver of the same vendor and type. Two pairs are masked today: the two ZWO ASIAIR switch drivers behind the ZWO dew-heater switch registration, and the Gemini Cover Lite class plus the Rev2 model path behind the Flat Panel Pro registration. Both are covered by code review only. **`scripts/check_stress_registration.py`'s docstring is the authoritative list** — this sentence is a pointer to it, not a second copy, because it has already gone stale once by naming only ZWO. **SDK-callback paths especially**: the TSan suppressions mute any report with a vendor-blob frame on the stack, so a race in driver code invoked from an SDK internal thread is invisible to CI unless that callback path is exercised through a fake-SDK seam (fully instrumented, no suppression applies) — when you add an SDK callback to a driver, register a fake-seam stress path for it in the same change.

- **A `std::thread` member that a failure path leaves joinable is a
  `std::terminate()` waiting for the destructor** (`Server`, issue #402). The
  shape: `start_async()` creates the thread, the thread's entry point fails
  early and sets `running_ = false`, and `stop()` early-returns on
  `if (!running_)` without joining. Nothing is wrong until `~Server()` destroys
  a joinable thread, or until a retry assigns over it — both call
  `std::terminate()`, so a recoverable failure (a port already in use) became
  an abort at a point far from its cause, and the caller's own `is_running()`
  check correctly said false and did not help. **Join on every path out,
  including the one that thinks there is nothing running**, and join before
  re-assigning. Audit any `if (!flag) return;` at the top of a teardown method
  for the same shape.

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
- **`ExpQHYCCDSingleFrame` and `GetQHYCCDSingleFrame` must run on the SAME thread, or `GetQHYCCDSingleFrame` never returns** (miniCam8M, real hardware, 2026-08). This bit us as a driver that appeared to "hang on exposure with no error" — every SDK call was already serialized against the same handle via a per-call mutex (see `QHYSDKWrapper::SharedHandle::call_mutex` below), which ruled out a concurrency bug, and QHY's own `SingleFrameMode.cpp` sample built and run standalone against the same camera completed cleanly, which ruled out an SDK/USB/firmware fault — the only remaining difference was that the sample calls `Exp`/`Get` back-to-back on `main()`'s thread, while this driver called `Exp` on the HTTP handler thread and `Get` on a separately-spawned worker. Fix: `start_exposure()` spawns exactly ONE worker thread that runs the *entire* arm→settle→download sequence, matching the sample's order (`SetQHYCCDResolution` before `SetQHYCCDBinMode`; `GetQHYCCDMemLength` fetched *after* arming, not before; a 1s settle delay before the download call when not `QHYCCD_READ_DIRECTLY`). Treat this as a hard constraint for any future QHY exposure-path change — do not reintroduce a second thread between `Exp` and `Get`, even for something as innocuous-looking as "record the arm time on this thread first."
- **"Linearity HDR" readout mode has a fixed ~64s download regardless of exposure duration — this is expected SDK behavior, not a hang** (miniCam8M, real hardware, 2026-08). `strace` during the download showed ~600 `USBDEVFS_SUBMITURB` calls paced at a consistent ~100ms interval, unaffected by `CONTROL_USBTRAFFIC` (tried 0 and 10) or `CONTROL_HDR` (tried 0 and 1) — the pacing is internal to `libqhyccd.so` for this mode specifically (reported to QHY; no documented parameter controls it as of SDK 26.06.04.16). A flat exposure-duration-based watchdog margin (this driver's was 15s, later 60s) will eventually kill a legitimate in-progress HDR download as "stuck" for large enough buffer sizes on slow modes. Fix: once `GetQHYCCDMemLength()` returns the actual buffer size, extend (never shrink) `exposure_deadline_` using a conservative minimum-throughput floor (`bytes / 500,000 B/s + 15s`) so the deadline scales with the readout mode instead of assuming every mode transfers at the same rate. Any new QHY readout mode with a large buffer should be expected to need the same treatment — don't assume Full Resolution's ~4s/36MB throughput applies uniformly.
- Temperature control requires `ControlQHYCCDTemp()` to be called approximately every second; use a dedicated background thread started/stopped with the cooler.
- `ControlQHYCCDGuide()` blocks the calling thread for the full pulse duration (confirmed on real miniCam8M hardware: a 2000ms pulse blocked the caller for exactly 2000ms) — run it on a detached thread (only the shared_ptr guiding flag, a copied camera ID, the pulse direction/duration, and a raw `QHYSDK*` for the SDK call itself — never `this`; see rule (b) above) so `PulseGuide` returns immediately, matching ASCOM's async expectation.
- `ControlQHYCCDTemp()` and `SetQHYCCDParam()` (at least for `MANULPWM`) have no SDK-side timeout and can occasionally run far past `ControlQHYCCDTemp`'s documented ~10s PID-loop figure. See the "blocking SDK call with no timeout" rule in Driver concurrency & lifecycle above — the temp-control and cooler-off worker joins in `qhy_camera_driver.cpp` are bounded (2s) and detach on timeout, and `qhy_sdk_wrapper.cpp`'s handle is reference-counted (`shared_ptr<qhyccd_handle>`) so that detach can never race a concurrent `close_camera()`.
- Guide direction convention differs from Alpaca: QHY uses EAST=0, NORTH=1, SOUTH=2, WEST=3 vs Alpaca North=0, South=1, East=2, West=3 — map explicitly.
- After changing readout mode, refresh chip info and reset ROI — sensor dimensions can change per mode.
- SDK global lifecycle (`InitQHYCCDResource` / `ReleaseQHYCCDResource`) is managed as a singleton in the wrapper; include `#define __CPP_MODE__ 1` before `#include <qhyccd.h>` in the wrapper `.cpp` only.
- Cameras require firmware files (`/lib/firmware/qhy/*.img` / `*.HEX`) in addition to udev rules. The udev rules call `fxload` to load firmware on plug-in, after which the device re-enumerates with a different USB product ID. Install firmware from `AlpacaCore/external/QHY/sdk_linux_arm64_26.06.04/lib/firmware/qhy/` to `/lib/firmware/qhy/`.
- The system `fxload` from apt does **not** support `-t fx3` (FX3-based cameras) and will exit 255 silently — always install the QHY SDK's own `fxload` binary from `sdk_linux_arm64_26.06.04/sbin/fxload` to `/sbin/fxload` instead.
- Re-enumeration in VMs: after `fxload` fires, the camera disconnects as `1618:c268` (Cypress WestBridge) and reconnects with its operational product ID. VMware and similar hypervisors will not automatically pass through the re-enumerated device unless the USB filter covers the entire QHYCCD vendor ID (`1618`). Test QHY cameras on bare metal or RPi rather than VMs where possible.
- **Integrated CFW (filter wheel) shares the camera's physical handle**: the miniCam8M and similar models have a color filter wheel accessed through the SAME `qhyccd_handle` as the camera — there is no separate CFW enumeration or `Open`/`Close`. `qhy_filterwheel_driver.cpp` is a second `AlpacaDriver` (device type `FilterWheel`) that resolves the SAME `cameraId`/`cameraIndex` as the paired camera device and calls the same `open_camera()`/`close_camera()`. Both drivers now reach those through the `QHYSDK&` seam (issue #321) rather than naming the singleton directly; production still resolves to `QHYSDKWrapper`, so the shared-handle behaviour below is unchanged. This required making the wrapper's handle map reference-counted (`Impl::SharedHandle{handle, open_count}`, keyed by camera_id): the first opener's `OpenQHYCCD` stays live and shared while either the camera driver or the CFW driver is connected, and only the last owner's `close_camera()` actually erases the entry and lets the `shared_ptr` deleter run `CloseQHYCCD` — mirrors ToupTek's `open_shared_by_id`/`close_shared` for its camera+thermal-switch pairing. **Any future QHY accessory that shares a camera's handle must go through `open_camera`/`close_camera`, never a raw `OpenQHYCCD`/`CloseQHYCCD`,** or it will silently steal/close the other owner's handle.
- **CFW does not need `InitQHYCCD`**: the 25.09.29 SDK's `testapp/common/ControlCFW.cpp` sample opened the camera and called `SendOrder2QHYCCDCFW`/`GetQHYCCDCFWStatus` directly with no `InitQHYCCD` in between, and this was validated on real miniCam8M hardware (ConformU clean run). The filter wheel driver therefore only calls `open_camera()`, not `init_camera()`, so it can connect and move the wheel even if the camera driver (which does call `init_camera()`) is never connected in the same session. Note the 26.06.04 SDK dropped that sample; its surviving `testapp/cmake_demo/test_cfw` demo uses a different style (`InitQHYCCD` + `Set/GetQHYCCDParam(CONTROL_CFWPORT)`), so re-validate the no-init SendOrder/Status path on hardware when bumping SDKs.
- **Every SDK call is serialized against its physical handle via `SharedHandle::call_mutex`** (miniCam8M, real hardware, 2026-08): `QHYSDKWrapper`'s handle map entry carries its own `shared_ptr<std::mutex>` in addition to the reference-counted handle, and every method that touches the SDK (temp control, telemetry, CFW, gain/offset, exposure arm/download, etc.) takes it for the full duration of the underlying vendor call, not just around the wrapper's own bookkeeping lock. This was added defensively while chasing the exposure hang above — real hardware showed even "instant" register reads (e.g. a temp-control poll) hang if they land concurrently with a handle that's mid-exposure. **`cancel_exposure()` deliberately does NOT take `call_mutex`** — it only briefly locks the wrapper's own map-lookup mutex — because its entire purpose is to interrupt a `GetQHYCCDSingleFrame` blocked on another thread; serializing it the same way as every other call would deadlock it behind the very call it needs to cancel.
- **CFW protocol is a single ASCII digit, not a raw byte**: `SendOrder2QHYCCDCFW(handle, &order, 1)` expects `order = '0' + position` (e.g. slot 3 → the character `'3'`), and `GetQHYCCDCFWStatus(handle, status)` reports the settled position the same way — `status[0]` is `'0'`..`'9'` when the wheel has arrived, and any other value (commonly non-digit) while it's still moving. That "still moving" case maps directly onto the ASCOM FilterWheel `Position` "-1 while moving" sentinel, so `QHYSDKWrapper::get_cfw_position()` returns `-1` for it, with no separate is-moving flag (same shape as ToupTek AFW's `FILTERWHEEL_POSITION`). The driver does NOT simply pass that value through, though: while a move is pending it masks *any* reading that does not equal the target to `-1`, so a wheel that reports a stale settled digit mid-transit still reads as moving. Only a reading equal to the target clears the pending state and updates the cache (`get_position()` in `AlpacaCore/src/vendors/qhy/qhy_filterwheel_driver.cpp`); pinned by the "reports -1 in transit then settles" case in `test_qhy_filterwheel.cpp`.
- **Slot count is hardware-reported, not user-fixed**: `GetQHYCCDParam(handle, CONTROL_CFWSLOTSNUM)` (control ID 44) returns the wheel's actual slot count at connect; `IsQHYCCDControlAvailable(handle, CONTROL_CFWPORT)` (control ID 17) detects whether a CFW is present at all — connecting the filter wheel device against a QHY camera with no CFW throws `NotConnected` rather than silently reporting a fake 0-slot wheel. The web UI's slot-count picker (5/7/8/9/Custom, matching QHY's CFW slot-count lineup) is config-only, purely to pre-seed filter names before the first connect — same convention as ZWO EFW/ToupTek AFW.
- **`GetQHYCCDCFWStatus` has no "moving" sentinel — it reports the wheel's ACTUAL passing position throughout the physical rotation** (ConformU finding on real miniCam8M hardware): unlike ToupTek's `FILTERWHEEL_POSITION` (which returns `-1` for the entire in-motion window), this SDK call returns whatever slot the wheel is currently near while it physically rotates through intermediate slots on the way to the target (observed sequence for a 4→3 move: `4→5→6→-1→0→1→2→3`, i.e. it went the "short way" round through 5/6/0/1/2, and only returned `-1` briefly at one ambiguous point) and only settles on the commanded value once truly arrived. Each raw call is also a genuine ~100-130ms hardware round trip, which blows ConformU's FAST (0.1s) target for the first `Position`/`DeviceState` read after `Connect`. **Do not cache the first post-move reading unconditionally** — an earlier version of this driver did exactly that (cache on any non-negative digit) and it froze `Position` at the stale pre-move slot forever, failing every ConformU move test with a 30s timeout, because literally every raw reading looks like a valid "settled" digit including the ones taken mid-rotation. The fix: (1) one warm-up read during `Connect` (charged against the STANDARD 1.0s budget) seeds a settled-position cache so the two connect-adjacent FAST-classified reads (`DeviceState`, the first `Position` Get) are served from cache; (2) `set_position()` records the commanded target and clears the cache; (3) while a target is pending, `get_position()` always does a live read, and if that read doesn't match the pending target it is **masked to `-1`** rather than passed through raw — a real client (NINA) polling mid-move otherwise sees the wheel's actual but unrelated transit slot (e.g. `5` during a `4→3` move) and reports it as a mismatched/erroneous arrival before the wheel has actually settled. Only once a live read equals the pending target do we cache it and resume serving from cache. See `qhy_filterwheel_driver.cpp`'s `get_position()`/`set_position()` for the implementation.

### SVBONY

Devices: Camera.

SDK location: `AlpacaCore/external/SVBONY/lib/armv8/`, headers under `external/SVBONY/include/`.

- **SC715C is a rebadged ToupTek G3M715C, NOT served by this driver.** The SVBONY SDK does not recognize the SC715C. Configure it with vendor `touptek` (device type Camera, `ALPACACORE_ENABLE_TOUPTEK`) — the ToupTek SDK enumerates it natively under its own model name `G3M715C`. Same rebadge pattern as the iOptron iCAM cameras being served by the Player One driver (see iOptron notes below) -- but with one difference that matters to the user: the router aliases vendor `ioptron` + camera onto the Player One driver, so an iCAM owner still selects `ioptron`. There is no `svbony` + camera alias (that vendor has its own driver), so the SC715C must be configured as `touptek`. Validated 2026-09-11 on Linux arm64: 0 errors, 0 issues, 0 timing issues (ConformU 4.5.1 — see the [ConformU 4.5.0 arm64 timing bug](#target-architecture) note if an earlier ConformU version shows spurious timing failures). Report saved at `AlpacaCore/conformu/SVBONY/SC715C/Linux-arm64.txt`.

- **Control warm-up at connect (SV905C2 quirk)**: After `SVBOpenCamera`, `SVBSetControlValue(SVB_GAIN, ...)` returns `SVB_ERROR_GENERAL_ERROR` indefinitely on SV905C2 — regardless of value, regardless of `bAuto` flag, regardless of whether `SVBStartVideoCapture` is active, and `SVBRestoreDefaultParam` does not clear the state. The driver works around this by iterating every writable control reported by `SVBGetControlCaps` and writing each to its `default_value` during the connect path (after `SVBSetROIFormat` / `SVBSetOutputImageType`). Once any `SVBSetControlValue` call has landed, subsequent client gain writes succeed. Failures during the warm-up are tolerated and logged at DEBUG. Do not remove the warm-up loop in `set_connected` without re-running ConformU against an SV905C2 — the failure is silent until a client tries to set gain. Likely related to SDK readme entries `v1.13.1: Fixup ASCOM software to support SV905C2` and `v1.13.2: Optimize gain settings of SV905C2`.
- **Auto control writes**: `disable_auto_if_needed` reads the current value/auto flag and only writes back if currently auto, since some SVBONY models reject manual writes while auto is active with the same `SVB_ERROR_GENERAL_ERROR`.
- **`SVBSetControlValue` retry**: The wrapper retries up to 3 times with a 50 ms backoff specifically on `SVB_ERROR_GENERAL_ERROR` to absorb genuinely transient hardware-op faults; deterministic rejections still surface after the retries are exhausted.
- **Camera mode**: We use `SVB_MODE_NORMAL` (continuous video) and start/stop `SVBStartVideoCapture` per exposure. INDI's `indi-svbony` driver instead uses `SVB_MODE_TRIG_SOFT` with persistent video capture for stills — keep this in mind if a future SVBONY model needs trigger-mode behavior.
- **Bin/ROI quirks**: divisors width%8, height%2 (see [Camera ROI alignment](#camera-roi-alignment-all-camera-vendors)). ROI updates and `FrameSpeedMode` writes are deferred to `start_exposure` because some SDK control writes take ~1.1 s and would otherwise blow ASCOM client timing budgets.
- **`SVBRestoreDefaultParam`** is called immediately after `SVBOpenCamera` to clear any leftover state from a previous session, mirroring `indi-svbony`. Tolerate failure for older SDK builds that don't export the symbol.

### ToupTek

Devices: Camera, Focuser (AAF — Astro Auto Focuser), FilterWheel (AFW — Astro Filter Wheel, AFW-M 5/7-slot), Switch (two backends: cooled-camera **Thermal** — dew heater + fan; and the **StellaVita PowerBox** — GPIO).

- **Rebadge note**: the camera sold as **SVBONY SC715C** is this same G3M715C hardware and enumerates via this driver's SDK under the name `G3M715C` — configure it with vendor `touptek`, not `svbony`. See the SVBONY section above.

SDK location: `AlpacaCore/external/ToupTek/toupcamsdk.20260128/` (shared between the camera, focuser, filter-wheel, and thermal-switch drivers). The StellaVita Switch driver uses **no SDK** — it is a libgpiod-only driver that happens to live under the ToupTek vendor.

- **Single SDK, multiple device types**: camera, focuser, filter-wheel, and thermal-switch drivers all go through `ToupTekSDKWrapper`. Cameras enumerate via `enumerate_cameras()`, focusers via `enumerate_focusers()` (filters `Toupcam_EnumV2` by `TOUPCAM_FLAG_AUTOFOCUSER`), filter wheels via `enumerate_filter_wheels()` (filters by `TOUPCAM_FLAG_FILTERWHEEL`). Same `Toupcam_Open` selects a camera/focuser/wheel by its capability flag. **`enumerate_cameras()` must EXCLUDE the accessory flags** (`TOUPCAM_FLAG_FILTERWHEEL | TOUPCAM_FLAG_AUTOFOCUSER`) — `Toupcam_EnumV2` returns AFW wheels and AAF focusers in the same list, and the camera driver resolves `cameraIndex` as a *position into the enumerate_cameras() vector* (then opens by that entry's id), so an unfiltered list makes `cameraIndex=0` silently open the filter wheel when both are attached. The three enumerations partition the devices: cameras = neither accessory flag.
- **Runtime sensor-register writes** (ToupTek mechanics of the exposure-guard rule in the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver)): the ToupTek camera is the *only* driver that programs sensor registers at runtime — CG/HFW (`put_cg`/`put_high_fullwell`), gain (`put_gain`), black level (`put_blacklevel`); the ZWO/SVBONY/Player One `set_readout_mode` are no-op stubs. `SetReadoutMode`/`Gain`/`Offset` do the `mutex_`-taking validation first (`handle_copy()`, ranges), then `lock(readout_mutex_)` → `ensure_connected()` → `ensure_not_exposing()` → SDK write. The geometry setters (`set_bin`/`set_num_x/y`/`set_start_x/y`) also take `readout_mutex_` + `ensure_not_exposing()` — not for the SDK (they only set `*_dirty_` flags) but because mutating `bin_`/`num_x_`/`start_*_` mid-integration desyncs the in-flight frame's geometry from the next buffer size.
- **Abort mechanics** (ToupTek mechanics of "wake the SDK wait before joining"): the exposure thread parks in `Toupcam_WaitImageV4` (the wrapper deliberately does NOT hold the SDK lock across it). `stop_exposure`/`stop_exposure_thread` call `sdk.stop(handle_)` **under `mutex_`** to unblock the wait before joining; that halts the pull-mode stream (started once at connect via `start_pull_mode`), so they set `format_dirty_`+`roi_dirty_` to force the next `start_exposure` to re-init it, and `stop_exposure_thread` runs *before* the ROI snapshot so that re-init lands in the same exposure. Don't pre-clear `exposure_active_` before the join — let the worker clear it via the stopped stream, else a concurrent register write sees a false "idle" mid-frame.
- **ROI dirty-flag timing** (ToupTek mechanics of "clear flags after validation"): `start_exposure` snapshots `format_dirty_`/`roi_dirty_`, validates the ROI, then clears them at the *end* of the locked block; the worker's catch re-marks *only* the stage (`format`/`roi`) that didn't complete (via local `*_applied` bools) so a failed apply doesn't force a needless stream restart.
- **Two device drivers can share ONE camera (reference-counted open)**: `Toupcam_Open` allows only one handle per physical camera, but the thermal switch (dew heater/fan) must operate the *same* camera the Camera device is streaming from. `ToupTekSDKWrapper` reference-counts opens by the device's opaque id (`shared_by_id_` / `id_by_handle_` maps) via the shared `Impl::open_shared_by_id` / `close_shared` helpers: `Toupcam_Open` fires only for the first opener and returns the shared `HToupcam`; `Toupcam_Close` fires only when the last holder releases. **ALL open-by-id paths route through these helpers — camera, focuser, AND filter wheel** (`open_camera_by_id`, `open_focuser_by_id`, `open_filter_wheel_by_id` all delegate); don't reintroduce a raw `Toupcam_Open`/`Close` in any of them. Distinct physical devices enumerate to distinct ids so they never collide, but two driver instances on the *same* id (the camera + its thermal switch, or a camera + its integrated autofocuser) now correctly share one open instead of the second raw-open returning null. Mirrors the Player One wrapper's `usage_`/`open_count`. Consequence: connecting the camera *and* the thermal switch is one physical open; disconnecting the camera while the switch is still connected keeps the camera powered/cooling (desirable). Opens by *index* (`open_camera_by_index`) are not tracked and close immediately (legacy path).
- **Offset = black level**: ASCOM `Offset` maps to `TOUPCAM_OPTION_BLACKLEVEL`, gated on `TOUPCAM_FLAG_BLACKLEVEL`. Integer `OffsetMin`(0)/`OffsetMax` mode (no named `Offsets` list). `OffsetMax` scales with the current output bit depth — `31 << (bits - 8)` (`TOUPCAM_BLACKLEVEL8_MAX` = 31), where bits is 8 in 8-bit output mode else the camera's deep bit count — so it's computed in the wrapper (`get_blacklevel_max`) which reads `OPTION_BITDEPTH`. ToupTek was previously the only camera driver stubbing offset to `PropertyNotImplemented`; it now matches ZWO/SVBONY/Player One/QHY. **`FullWellCapacity` is NOT queryable from the SDK** — the driver returns the ADU saturation (`2^bitdepth − 1`), not electrons; the true full well is a sensor datasheet spec (IMX571: ~51 ke⁻ Normal, ~100 ke⁻ High Full Well), and the High Full Well ReadoutMode is what switches between them.
- **Conversion gain + High Full Well → ReadoutModes**: the two ToupTek sensor-mode axes — conversion gain (`TOUPCAM_OPTION_CG`: 0=LCG, 1=HCG, 2=HDR-if-`FLAG_CGHDR`) and High Full Well (`TOUPCAM_OPTION_HIGH_FULLWELL`) — are folded into ONE flat ASCOM `ReadoutModes` list (ASCOM has only one readout-mode axis). `readout_mode_specs()` builds the list from capabilities: `HCG/LCG(/HDR)/High Full Well` when both, `HCG/LCG(/HDR)` for CG-only, `Normal/High Full Well` for HFW-only, else `Normal`. **Each spec fully specifies BOTH axes** (e.g. "High Full Well" = CG-LCG + HFW-on) so `get_readout_mode` round-trips to a stable index by reading both options. This is the idiomatic ASCOM home for hardware sensor modes (NINA shows a dropdown), NOT a custom Action. Both `get_readout_modes()` (the list) and `get_readout_mode()` (the current index) throw `NotConnected` while disconnected (ASCOM contract — keep them consistent), while `set_readout_mode` validates the range *before* the connection check so an out-of-range index is `InvalidValue` even disconnected. `preload_camera_info()` populates caps at *construction*, so a unit test must NOT assert a specific mode list (it depends on the attached camera); assert only the hardware-independent invariants — both getters throw `NotConnected` disconnected, and out-of-range `set_readout_mode` throws `InvalidValue`.
- **Odd bin factors need an even ROI span (3×3 hang)**: `Toupcam_put_Roi` coordinates are in ORIGINAL (sensor) resolution for digital binning (SDK header note (a)), and the SDK requires **even** width/height/offset. The binned ROI span is `num × bin`; for an odd bin factor that product can be odd (full-frame 3×3 on the ATR2600M → 4167-tall), which `put_Roi` rejects — the exposure then hangs and `ImageReady` never sets (2×2/4×4 are always even, so only 3×3 fails ConformU). Fix in `start_exposure`: round the sensor span UP to even and the offset DOWN to even; digital binning floor-bins the padded span back to exactly `num` pixels (the +1 pad is `< bin` for any `bin ≥ 2`), so the buffer and reported `NumX`/`NumY` stay correct. **Crucially, derive the max binned dimension from the EVEN sensor size** (`(max_width & ~1) / bin`, not `max_width / bin`): the largest deliverable binned width is `floor(even_max/bin)`, so a client requesting `floor(raw/bin)` on an odd-width sensor can't ask for a span that, once even-rounded, exceeds the sensor and clamps back to fewer than `num` columns (a zero-filled black edge column). With the limit derived from the even size, `ceil_even(num×bin) ≤ even_max` always holds and the clamp is unreachable. Keep a small buffer margin as crash-insurance.
- **Thermal reads come from a background poller, never the request path (cooled cameras)**: `Toupcam_get_Temperature` and each TEC `get_Option` are synchronous USB control transfers (~50-70 ms on a Pi). `DeviceState` reads CCDTemperature + CoolerPower + HeatSinkTemperature in one call, i.e. 3 transfers ≈ 0.13-0.18 s against the 0.1 s FAST target (ATR585M, 3.5.1). An on-demand TTL cache does NOT fix this: ConformU times the *first* DeviceState after connect, when any lazy cache is cold. The camera driver runs `thermal_poll_loop` (1 s cadence, primes the cache before the client's first poll; `TEC_VOLTAGE_MAX` read once per connection) and the getters serve the cache (`thermal_cache_fresh_locked`). Lifecycle rules learned the hard way: (1) spawn and join go under `exposure_lifecycle_mutex_` (taken at the top of `set_connected` in both directions) — an unguarded join racing a concurrent connect's `std::thread` assignment corrupted the handle and hung `join()` forever in the reconnect stress test; (2) never join the poller while holding `mutex_` (it takes `mutex_` every tick); (3) a redundant Connect must leave the poller running. (4) **No SDK traffic during an exposure**: thermal control transfers issued while `Toupcam_WaitImageV4` is pending made the SDK return `E_UNEXPECTED` and drop the frame at bin 3-4 (ConformU: "timed out waiting for camera to leave the 'Exposing' state", then ~60 cascading ISSUEs) — the poller skips ticks while `exposure_active_` and the getters serve the last reading regardless of age until the exposure ends. Apply the same shape to any other camera whose thermal reads are slow control transfers.
- **Thermal switch (dew heater + fan + tail LED)** — `touptek_thermal_switch_driver.{h,cpp}`, mirroring `playerone_switch_driver`. Dew heater = `TOUPCAM_OPTION_HEAT` (level 0..`OPTION_HEAT_MAX`), fan = `TOUPCAM_OPTION_FAN` (speed 0..`model->maxfanspeed` — the fan max comes from the enumerated model, not an option), tail indicator LED = `TOUPCAM_OPTION_TAILLIGHT` (boolean on/off; astro users turn it off to avoid reflections/light leaks). Heater/fan are capability-probed via `TOUPCAM_FLAG_HEAT`/`_FAN`; the **tail LED has no capability flag**, so it is probed by *reading* `get_taillight` in a try/catch and only exposed if the camera accepts it. A camera exposing none of the three throws `NotImplemented`. `kMaxThermalElements` = 3 (the disconnected switch-ID bound). **The cooler is NOT a switch element** — it lives on the Camera interface (`CoolerOn`/`SetCCDTemperature`/`CoolerPower`), matching ASCOM and Player One. The `(touptek, switch)` router branch and the config sanitizer pick the backend from `switchType`: `"thermal"` (bound by `cameraIndex`) vs `"stellavita"` (default, GPIO). The thermal switch builds on any ToupTek host (camera SDK only); StellaVita still needs libgpiod. **Gotcha (bit us):** the web-UI switch-type `<select>` must use `name="touptekSwitchType"`, not the bare `switchType` — ZWO's hidden select wins the FormData collision and the ToupTek switch silently registers as StellaVita (fails to connect: no GPIO). The discriminator-select instance of the [Enumeration index fields](#enumeration-index-fields--unique-names--auto-numbering-all-vendors) rule.
- **AFW (Astro Filter Wheel) — option-based, no dedicated API**: unlike the AAF focuser (which has the `Toupcam_AAF` action interface), the filter wheel is driven through the generic `Toupcam_get/put_Option`. Slot count = `TOUPCAM_OPTION_FILTERWHEEL_SLOT` (read once at connect, like ZWO EFW's `slotNum` — never hardcode 5/7; the web UI's 5/7/Custom picker is config-only). Position = `TOUPCAM_OPTION_FILTERWHEEL_POSITION`: **get returns `-1` while in motion** — that maps *directly* onto the ASCOM FilterWheel `Position` "moving" sentinel, so pass it through unchanged (no extra is-moving flag needed). On set, the low byte is the target slot (mask with `& 0xff`) and `(val>>8)&0x1` is a direction bit (`0` = clockwise, `1` = "auto direction"). **The wheel MUST be homed at connect or it hunts and never lands** — the single most important thing about this driver, and it bit us hard: without homing, moves make the wheel tick/rock in place ("tick-tick pause, never completes a revolution"), especially right after a firmware update (the firmware loses its slot reference). The fix mirrors the **INDI `indi_toupwheel` reference driver's** connect sequence exactly: (1) `get_Option(FILTERWHEEL_SLOT)` read the slot count; (2) `put_Option(FILTERWHEEL_SLOT, slot)` write it straight back (re-applies the wheel's slot config; the option is `[RW]`); (3) `put_Option(FILTERWHEEL_POSITION, -1)` home/reset so the firmware references its slots (in INDI this is `SelectFilter(0)` → `put_Option(POSITION, SpinningDirection | (0-1))` = `-1`). `reset_filter_wheel` returns immediately but the firmware takes ~1.5 s to home — **wait for the home to settle (poll `get_filter_wheel_position` until it returns a non-negative slot; it reports `-1` while moving) BEFORE reporting `Connected`**, otherwise a `SetPosition` arriving during the home window aborts the cycle and leaves the slot reference unknown (moves then land on wrong slots — the exact failure homing was added to prevent). Once homed, a **single absolute move** (`put_Option(POSITION, position)`, 0-based, direction bit `0`) traverses to any slot fine — do NOT step one slot at a time, and do NOT set the direction bit to `1`/auto (that made single-slot moves oscillate in place on our hardware). ASCOM `Position` is already 0-based so no `±1` offset is needed (INDI carries a `-1`/`+1` only because it is internally 1-based). The wrapper hides these option constants inside `touptek_sdk_wrapper.cpp` (same as the camera `TOUPCAM_OPTION_*` calls) so driver code never includes `toupcam.h`.
- **AFW driver mirrors ZWO EFW for filter semantics, ToupTek focuser for handle/connection**: `Names`/`FocusOffsets` normalization (length tied to slot count, single-string-to-chars expansion, default `Filter N` names) is copied verbatim from `zwo_filterwheel_driver.cpp`; the `HToupcam handle_` + async connection-thread lifecycle is copied from `touptek_focuser_driver.cpp`. Constructed by index or SDK id (string), same as the ToupTek focuser — not the ZWO `int wheel_id`.
- **AAF API convention** (`Toupcam_AAF(handle, action, value, *out)`):
  - **SET**: `Toupcam_AAF(h, AAF_SETxxx, value, nullptr)` — passes value in third arg.
  - **GET**: `Toupcam_AAF(h, AAF_GETxxx, 0, &out)` — third arg is unused, output via pointer.
  - **RANGE-of-GET**: `Toupcam_AAF(h, AAF_RANGEMAX, AAF_GETxxx, &out)` — queries the upper bound of a GET property by passing the GET action code as the third arg. Used to discover MaxStep and Backlash range at connect time. Same pattern works for `RANGEMIN` and `RANGEDEF`.
  - **HALT/SETZERO**: control actions, third arg is the new value (0 for halt; ticks for setzero/sync).
- **`AAF_GETSTEPSIZE` is mechanically meaningful only for specific focuser configurations** — the driver does not expose it as ASCOM `StepSize`. Connected, it throws `PropertyNotImplemented` per the focuser ASCOM contract; disconnected, the connection check answers first and it is `NotConnected` (#309).
- **Temperature units**: `AAF_GETTEMP` returns tenths of Celsius (e.g. `32` → `3.2 °C`). Divide by 10.0 before returning to ASCOM. INDI applies a 0.1 °C hysteresis when updating UI; we read on demand so the hysteresis is unnecessary on the driver side.
- **No temp-comp action**: The AAF action set has no temp-comp control. Connected, `TempCompAvailable` returns false and `set_temp_comp(true)` throws `NotImplemented` (not `DriverException`); disconnected, all of them are `NotConnected` (#309).
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
- **Connect verifies the link with the protocol echo (`K` + byte → byte + `#`)** (issue #130): `connect()` only opens the port, so before this gate a port with nothing listening came up as `Connected=true` once every handshake query (firmware, model, site, time, RA/Dec, Alt/Az) had burnt its 5 s response timeout inside `catch (...)`, and every command then timed out too. Silence on the echo fails the connect within one timeout with a message naming the port. A mismatched reply is NOT accepted (PR #3 review: a port that merely answers something framed like the protocol would otherwise proceed into the swallowed handshake queries and reproduce the phantom link) — `echo_test()` first reads past it for the rest of one response timeout, because right after `connect_serial()` opens the port the first token can be a stale reply from the previous session (`connect_serial()` flushes the tty buffer, but a USB adapter's own FIFO can still deliver one), then resends the echo once for a handset that garbled a byte, and fails after a second mismatch. Regression tests for silence, garbled-once, garbled-twice and stale-token-ahead-of-echo are in `test_synscan_async_park.cpp`; the shared `FakeMountServer::default_responder()` answers the echo so the concurrency stress tests still connect (they assert it). `connected_` is atomic and `get_connected()` lock-free (the connect sequence holds `mutex_` throughout). Every handset command and reply is logged at TRACE (`HC <cmd> -> <reply>`; non-printable bytes as `\xNN`).
- **A silent handset is usually a wedged handset, not wiring** (EQM-35 Pro rig, SynScan V4 fw 04.40.00, 2026-09): after ANY bytes at the wrong baud (115200 from the Sky-Watcher direct-USB probe or a manual test) the handset answers nothing — both protocols, every baud, every DTR/RTS state — until power-cycled, and a mount restart does not reboot a handset that is USB-powered from the SBC (no re-enumeration in `dmesg` is the tell). Recovery: unplug USB from the SBC → power-cycle the mount → handset to its main screen → plug USB back in. Healthy, it answers `K`+byte with byte+`#` at 9600, auto-detect finds it (`HC firmware 04.40.00`), connect takes 0.6 s and reads 2–35 ms. Never send non-9600 traffic to a port that may be a handset; see `util/synscan_handset_probe.h`.
- **Pulse guiding**: SynScan V3/V4 protocol has no hardware pulse guide command. Driver implements software-timed variable-rate slew: issues a variable-rate axis slew at the guide rate, sleeps for the requested duration, then stops the axis and restores sidereal tracking. `IsPulseGuiding` tracks completion via time-based end time plus delay.
- **GEM pier-side DEC direction flip**: DEC motor direction is inverted when the mount's pointing state is 'W' (west), matching the physical axis reversal on German equatorial mounts. This affects pulse guide and MoveAxis DEC commands.
- **Position override accumulation**: Instead of reading back noisy mount positions after tiny guide pulses, the driver accumulates expected `rate × duration` deltas directly into the target coordinate frame. All consecutive pulse guide directions (N/S/E/W) operate in the same coordinate baseline, eliminating drift between reads.
- **RA tracking restoration**: The stop thread re-issues `set_tracking_mode()` after stopping an RA-axis pulse to counteract the variable-rate stop command killing sidereal tracking. Without this, the mount stops tracking after every RA pulse guide.
- ConformU 4.3.0 validated for **Sky-Watcher HEQ5 PRO** on Linux arm64 with 0 errors and 0 issues.

### SkyWatcher (Wave / direct motor controller)

Devices: Telescope. Vendor key `skywatcher` — distinct from `synscan`, which speaks the
hand-controller protocol. This driver speaks the **Sky-Watcher motor controller command
set** (the `:` command / `=`|`!` reply protocol) directly to the mount's motor board.
Target hardware: Wave 100i/150i (also applicable to AZ-GTi-class mounts).

Protocol documentation: `AlpacaCore/external/SynScan/SkyWatcher_Motor_Controller_Command_Set.md`
(shared with the SynScan vendor directory). No external SDK required.

Connection types: Serial (the mount's own USB port or an EQDIR-class adapter, 8N1; the scan probes
9600 then 115200 per port, because an EQ board's built-in PL2303 port answers only at 115200,
so a silent Prolific/FTDI/CH340-class port costs at least about 3.3 s per scan: 1.5 s at 9600, the
300 ms SynScan echo guard, 1.5 s at 115200, and up to roughly 4.4 s because the read loops only
check their deadline between `VTIME` reads; multiplied by every such adapter on the rig; #403
records the measurement) and Network (built-in Wi-Fi module,
**UDP** port 11880 — one command per datagram, one reply per datagram; AP-mode address
192.168.4.1). The wrapper retransmits up to 3 times on UDP timeout and drains stale
datagrams before each send so replies cannot get off-by-one.

- **The serial probe asks for a SynScan handset echo first and skips the port if one answers** (`util/synscan_handset_probe.h`, 2026-09): a SynScan V4 hand controller (fw 04.40.00, built-in PL2303 `067b:23a3`) shares the Prolific adapter class this scan targets, and it stops answering serial ENTIRELY after receiving bytes at the wrong rate — one motor-controller probe at 115200 is enough — until it is power-cycled (unplugging the mount is not enough when the handset runs on USB power from the SBC). On the EQM-35 Pro rig this was the whole "hand-controller commands time out" report: the handset had been wedged by this probe at service start. The guard is at the top of `probe_skywatcher_port()`, gated on the caller's baud not being 9600 (the rate that is safe for a handset to receive). Since the EQ-class support landed, `kProbeBauds` includes 115200, so the guard fires on every scanned port that stays silent at 9600 and costs its 300 ms echo timeout there; it still covers every future non-9600 caller; the same hazard applies to any other scan that sends non-9600 traffic to Prolific-class ports (the iOptron iEAF/iAFS2/3 and iEFW handshakes at 115200 are the known ones — not yet guarded). The SynScan driver's serial connect additionally claims the port with `TIOCEXCL`, an independent layer that blocks a concurrent same-process open regardless of baud. Hardware-verified 2026-09-09 (EQM-35 Pro rig): the exact-echo path connects cleanly, a manual open of the port while connected fails with `EBUSY` and succeeds again immediately after disconnect (no lock leak), and an abrupt `systemctl restart` mid-poll still re-detects the handset on the very first probe after restart (the closest this rig can reproduce of the stale-reply race the tolerant read loop targets). **Not exercised by a hardware pass yet:** the baud gate firing at 115200 — the fix's own validation branch only ever called `probe_skywatcher_port()` at 9600; its trigger (`kProbeBauds` including 115200) is now on `main`, so the combined guard-plus-115200 path still needs its own hardware pass on the EQM-35 Pro rig. **Reverse direction (review on open-astro#242):** the guard sends the 9600 echo to a port that may be a Sky-Watcher motor board expecting 115200 — could a board be wedged by wrong-rate bytes the way the handset is? Empirically no, and the guard adds no new class of traffic: on `main` the probe has always sent `:e1` at 9600 to every candidate port, and `driver/skywatcher-eqm35`'s `kProbeBauds` tries 9600 first, so the real EQM-35 board received 9600 traffic before every successful 115200 detection during that branch's hardware bring-up (build a014531). The guard only runs on the non-9600 pass, i.e. after that same port has just been probed at 9600. Still, when the branches are combined, the combined pass should confirm both directions on the same rig: handset not wedged by the scan, board still detected at 115200 after the 9600 echo.
- **Coordinates are equinox of date, not J2000.** `EquatorialSystem` reports Topocentric and
  RA/Dec come from LST, so they are mean-equinox-of-date; plate solvers return J2000, and the
  two drift apart by ~50"/yr since 2000 (~22 arcmin in 2026, mostly RA). Alpaca clients read
  the flag and convert; a raw solver output held next to the driver's reported RA/Dec does not,
  and a `SyncToCoordinates` fed a J2000 position writes the whole offset into the mount.
  Precess before comparing or syncing, and rule this out before reading a ~20' goto error as
  a driver bug (open-astro#230, Wave 150i report).
- **All pointing math lives in the driver.** The MC protocol only counts steps: the driver
  owns RA/Dec <-> axis-angle conversion (CPR read at connect via `:a`, timer frequency
  `:b`, high-speed ratio `:g`), LST computation, pier-side selection, and tracking-rate
  step-period math (`T1 = TMR_Freq * 360 / rate / CPR`, times the high-speed ratio in
  fast mode). The mount stores **no site or time** — site lat/long/elevation come from
  the web UI config or the Alpaca setters. **Latitude and longitude are mandatory on this
  vendor** (#274): `configuredevice` rejects a skywatcher config without both, and
  `Connected = true` throws `InvalidOperation` unless each has been set explicitly, by
  config or by its setter. A config **already on disk** is registered anyway, with a WARN,
  and left for the connect-time guard to refuse: a device dropped at startup never enters
  the registry, so `configureddevices` cannot list it and the web UI offers no way to edit
  the entry that is at fault. That asymmetry is the rule for any new validation in
  `register_device_from_config` — reject `ConfigSource::Api`, warn on `ConfigSource::Persisted`.
  Since #380 that rule is not left to each branch to remember: `Router::reject_invalid_config()`
  takes the source and the reason and returns whether the caller must refuse, and
  `Router::normalize_persisted_connection_type()` does the same for an unrecognised
  `connectionType`, which has no value to carry forward — it returns `"serial"` for a persisted
  config, never `"auto"`, so the connect fails on the port path instead of auto-probing and
  attaching to whatever mount answers. Use them rather than an inline `return false`; the
  `portPath`, `host` and `connectionType` checks in every telescope branch do.
  Both coordinates are also **range-checked** (#398), inclusive of ±90/±180 since the poles and
  the antimeridian are real places, and rejecting NaN and the infinities: presence alone let a
  config carry latitude 200, which reads as northern to `hemisphere_south_locked()`, while the
  ASCOM setters have always refused exactly that at runtime — a validation a client cannot bypass
  but a config can is not a validation. The reads and the check live in one shared
  `read_site_coordinates()` used by all seven vendor branches that take a site, and on the
  persisted path the offending coordinate is **cleared** so the driver's unset handling covers it. `0.0` is a real coordinate, so the driver tracks whether each
  was ever set rather than testing for the value — an unset southern rig would otherwise
  run northern pointing math: the #432 sky frame (both the `a1` term and dec), the RA
  tracking direction (#250, restored by #432) and the Dec rate / pulse-guide sign (#253).
  **Not #261**, despite what this line said before #432 and what the `#274` CHANGELOG entry
  still says as history: the pier-side branch and label are picked from the sky hour angle
  and are hemisphere-independent, which is one of #432's findings. The driver comment on the
  connect-time guard says the same. Time comes from two functions: `utc_now_locked()`
  feeds every LST computation (pointing, `SiderealTime`, pier side, gotos) and applies the
  client-set `UTCDate` offset only while the host clock is undisciplined (no NTP): sampled at
  the write and, while such an offset is armed, re-sampled at most once per 30 s on the pointing
  path through `detail::host_synchronized_probe()` (one `adjtimex` read, no device I/O; #405), so
  a client's clock error never steers pointing on an NTP-good host and stops steering it within
  about 30 s of the host becoming disciplined by slewing (INFO log; the flag only moves
  undisciplined to disciplined, since ignoring the offset is the safe side);
  `client_utc_now_locked()` feeds the `UTCDate` readback and always honours the client's write,
  because that property is the client's to set and ConformU reads back what it wrote (#287,
  #351). On an NTP-less host the router also steps the system clock from that write (#289). The
  offset is not sticky: it is dropped (with an INFO log) as soon as the host clock is stepped
  underneath it (Sync Time, NTP taking over, `date`), detected as the system and steady clocks
  disagreeing by more than 1 s since the write, and re-armed by the next `UTCDate` write; the
  30 s re-sample above covers discipline gained without a step. Tests pin both branches through
  the probe seam (`ProbeGuard` in `test_skywatcher_async.cpp`) rather than the build host's own
  clock state (#395). **This split is Sky-Watcher-only.** A mount with its own clock (OnStep,
  Celestron, SynScan, iOptron, ZWO AM) has the ASCOM `UTCDate` setter write the MOUNT's time,
  and its goto and sidereal logic then run on that clock, so those drivers keep aiming by the
  client's instant on purpose; what they share with #301 is the once-per-connection WARN when
  an NTP-disciplined host disagrees with the client by more than
  `HostClock::kClientDisagreementWarn`, through `alpacacore/util/client_utc_warning.h`
  (`ClientUtcWarning::warn_once()` after the write, flag re-armed on connect, probe seam
  `set_host_synchronized_probe()` for tests; #409). A new driver that caches a client-set time
  the same way calls it too.
- Pointing convention (#432): home = counterweight down, tube parallel to the polar axis
  pointing at the visible pole, counts offset `0x800000`, axis angles `a1`/`a2` in degrees
  from home in the increasing-count direction. **`HA = s * (a1/15) + (a2 >= 0 ? +6 h : -6 h)`
  and `dec = s * (90 - |a2|)`, with `s = +1` north and `-1` south.** The 6 h term is the
  counterweight-down home: the dec axis lies in the meridian plane there, so a dec-only
  rotation sweeps the HA = ±6 h circle and the meridian needs the bar horizontal
  (`a1 = ±90`); every reachable target keeps `|a1| <= 90`, which is the
  counterweight-never-above-horizontal rule falling out of the geometry. Its SIGN follows
  which side of the dec axis the tube is on and does NOT flip with hemisphere; the `a1`
  term does, because the mount faces the other pole. **That asymmetry is measured, not
  derived, and #458 is open on it**: geometry says the 6 h term must flip too, and the
  two mounts it was fitted to (EQM-35 Pro south, Wave 150i north) cannot separate a
  hemisphere effect from a per-board dec-axis count sense. Pier side is hemisphere-independent
  (`a2 >= 0` -> pierEast), since the goto picks the branch from the sky hour angle.
  Tracking, `RightAscensionRate` and East/West pulses go through `ra_axis_sign_locked()`
  (counts up north, down south); `MoveAxis`, goto deltas and AutoHome are mechanical and
  never apply it. This matches `indi-eqmod`'s `EncodersToRADec()` exactly in the north;
  in the south the two differ by 12 h and a pier label, and the hardware backs this one.
  **Do not judge this model by the driver's own reported RA/Dec, ConformU included: the
  driver reports what it commands.** It was established by driving an EQM-35 Pro to known
  axis positions and reading the tube's real direction off the mount (2026-09-12); those
  rows are in the driver comment and asserted in `test_skywatcher_pointing.cpp`. Extend
  that file with a new hardware row for any change here.
- **Sync** uses the controller's own `:E` set-position command (motors must be fully
  stopped — the driver pauses tracking around the write), never a driver-side offset.
- **Pulse guiding**: RA pulses while tracking are done by changing the RA step period
  in-place (`:I` is legal during slow-mode motion), then restoring the sidereal preset —
  the axis never stops. Dec pulses (and RA while not tracking) are software-timed
  speed-mode nudges. Position override accumulation as per the SynScan lessons.
- **A live `:I` on a running axis is not always applied** (EQM-35 Pro, MC firmware 3.39,
  2026-09-06): the board stores the preset (`:i` reads it back) but the motor keeps its old
  rate. Every live in-place `:I` is therefore followed by a `:J` re-latch (INDI does the
  same), and the driver sample-verifies the rate over ~450 ms (`verify_live_rate_or_rekick`)
  and resends `:I`+`:J` if the axis did not change speed. Pulses ≥ 1.5 s verify inside the
  pulse task (the window is deducted from the pulse; shorter pulses rely on the kick alone);
  the `RightAscensionRate`/`TrackingRate` setters cannot wait 450 ms inside a property call,
  so they spawn a one-shot background task (`rate_verify_thread_`, open-astro #248). That
  task never takes `mutex_`, which is what lets every RA-taking path reap it WITH `mutex_`
  held (setters, Tracking off, `stop_axis_and_wait_locked`, pulse dispatch, AbortSlew,
  disconnect) — a lock-free reap would leave a window for a setter to spawn one between an
  initiator's reap and its lock, and the resend would land mid-pulse or on a stopped axis.
- `:f` status nibbles: char0 bit0 speed-mode/bit1 CCW/bit2 fast; char1 bit0 running/bit1
  blocked; char2 bit0 init-done/bit1 level switch. Slewing = running AND NOT speed-mode
  on either axis (a tracking axis is not slewing).
- Connect sequence: `:e` version, `:a`/`:b`/`:g` per axis, then `:F` init (with `:E` home
  stamp) ONLY when the status reports not-initialized — never re-stamp an aligned session.
- **Wave USB port is STM32 CDC-ACM** (`0483:5740`, `/dev/ttyACM*`, by-id name
  `usb-STMicroelectronics_STM32_Virtual_ComPort_...`), NOT a ttyUSB bridge chip. The
  auto-detect scan must include STM32/STMicroelectronics in the candidate filters and probe
  `/dev/ttyACM0-9` as well as `/dev/ttyUSB0-9`. Baud rate is irrelevant on CDC-ACM.
  Hardware-verified: `:e1` on the Wave 100i replies `=033A44` (MC firmware 3.58.68).
- **Park and MoveAxis(axis, 0) are asynchronous initiators** (ConformU 4.5 STANDARD timing,
  1 s target): a blocking park slew (19 s) and a blocking stop-and-wait in MoveAxis(0)
  (1.2 s deceleration ramp) both failed timing on real Wave 100i hardware. Park dispatches
  the slew in the background (AtPark turns true on completion); MoveAxis(0) issues the stop,
  keeps Slewing true via the manual flag, and a background task clears it and restores
  tracking once the axis reports stopped. This applies to every telescope driver.
- ConformU needs a real site. Since #274 a Sky-Watcher device with no site refuses
  `Connected` outright, so the run fails at connect. Since #358 the client is told why:
  the router reports the driver's own sentence naming the two fields as the
  `ErrorMessage` (the error number is still `NotConnected`), rather than a bare
  "Connection failed" with the reason left in the server log.
  Set the observing site in the web UI before validating. Before #274 the site collapsed
  to 0,0 instead and the CheckMethods slew tests aborted with "highest elevation
  available is below the horizon".
- Web UI: `skywatcher`-prefixed field names; network field is `udpPort` (NOT `tcpPort`).
- ConformU 4.5.0 validated on Wave 100i over **both transports** (Linux arm64): USB (dev PC)
  and Wi-Fi UDP (Raspberry Pi CM4 joined to the mount AP) — 0 errors, 0 issues, 0 timing
  violations each; slews within the ±10 arcsec tolerance.
- **Async-initiator self-deadlock trap (Park)**: making `Slewing` report true while
  `parking_` (so pollers never see the Slewing-false/AtPark-false gap) breaks the park
  task itself if its completion wait polls the same accessor — it can never observe
  "stopped" and times out (ConformU: "Failed to park within 300 seconds"). The internal
  wait must poll a hardware-only variant (`get_hardware_slewing_locked`).
- **PulseGuide is also an async initiator**: the axis dispatch (stop-and-wait on a
  ramping axis + possible UDP retries) took 1.79 s synchronously; it now runs inside the
  background pulse task with `IsPulseGuiding` already true at return.
- **AutoHome (home index sensors)**: the Wave reports feature bit 0x04 (":q" data
  0x000001 -> 0x100C) on both axes. FindHome ports the EQMod AutoHome procedure:
  arm the indexer (":W" data 0x000008), read it (":q" data 0x000000 -> 0 below /
  0xFFFFFF above / latched count), hunt the edge, approach from below, then
  `:E`-stamp kHomeCounts at the sensed mark. Count-frame home (goto 0,0) is the
  fallback for boards without the bit — and is NOT physically meaningful unless
  the mount was powered on at home (a killed ConformU run mid-sync can shift the
  frame; this is why AutoHome matters).
- **Tracking rates**: Lunar/Solar are just different step-period constants
  (live `:I` change while tracking). **RA/Dec rate OFFSETS are supported**
  (issue #214): RightAscensionRate is SUBTRACTED from the drive rate
  (RA = LST − HA), DeclinationRate flips sign on the east branch (a2 ≥ 0,
  dec = 90 − a2), sub-floor Dec rates duty-cycle floor-rate bursts on a 3 s
  period (~140 ms stop-landing compensation), reads hold the dead-reckoned
  model while offsets run, and offsets zero on a drive-rate change (setters
  throw InvalidOperation off Sidereal). The two "hardware anomalies" that
  originally deferred this (Dec undershoot 40→16 as/s; ~45 arcsec RA count
  jumps after in-place `:I` writes) were bench-DISPROVEN on 2026-08-23: direct
  UDP measurements show Dec tracks 5–320 as/s within 0.2% and zero `:j` count
  glitches across 120 reads interleaved with `:I` writes. Both symptoms were
  artifacts of the pre-#216 refinement-goto races, not the motor controller.
- **No read freezes — ever**: the old 10 s post-slew/post-sync/pulse position
  overrides masked a real GOTO landing error (~3 arcmin east: axis targets were
  computed with LST at dispatch, not arrival) and corrupted every ConformU 4.5
  endpoint measurement (rates, pulse displacement, sync return). Cures that
  replaced them: (1) gotos aim at the ARRIVAL-time LST and refine to an
  8 arcsec deadband; (2) reads dead-reckon `cached + commanded rate x elapsed`
  between hardware polls (kills the LST-vs-stale-cache sawtooth and count
  quantization); (3) LST uses sub-second time (whole-second truncation stepped
  RA in 15 arcsec jumps); (4) sync computes its frame AFTER the axes stop,
  aimed at the tracking-restart moment (a pre-stop frame is stale by the whole
  1-3 s pause -- ConformU saw a constant ~79 arcsec return error).
- ConformU 4.5 **physically measures pulse-guide displacement** (Dec moved,
  RA unchanged) -- a driver that freezes reads during the pulse fails with
  "The declination axis did not move".
- **Slewing must be a STATE FLAG spanning the whole goto + landing refinement**
  (`goto_in_progress_`, same pattern as `parking_`/`homing_`), never a timed
  hold: the 3 s `slew_force_until_` expired during a slow refine iteration
  (axis stop-waits take seconds), Slewing flickered false, ConformU started
  its pulse test, and the next refinement goto dragged the axes back to the
  slew target ("declination axis did not move", phantom RA drift, the
  constant ~79 arcsec sync-return error). Diagnosed by logging every motion
  frame (:G/:I/:J/:K) at WARN and killing ConformU at the first issue -- the
  trace showed three refinement gotos interleaved with the pulse.
- **Reap the pulse task at every motion boundary** (slews, park, home,
  moveaxis, sync, abort): ConformU's dual-axis pulse test leaves a live pulse
  timer that otherwise fires its stop/step-period restore into the middle of
  the next goto. A CANCELLED pulse task must not touch the hardware -- the
  canceller stops or re-commands the axes itself.
- **AbortSlew must cancel the async slew task** (set `slew_task_cancel_`,
  join later via reap) or the landing refinement re-slews after the abort;
  every slew entry point reaps first, which also resets the flag.
- Debug technique: a watchdog loop that `pkill`s ConformU at the FIRST logged
  issue preserves the exact journal window and stops the mount from grinding
  through a failed run.
- The shipped images log at WARNING: `ALPACA_LOG_INFO` never reaches
  journalctl on the test rigs -- temporary debug instrumentation must log at
  WARN or it silently vanishes.
- **CM4 `ondemand` CPU governor causes ~100 ms single-member FAST blips**
  early in ConformU runs (first request burst pays the clock ramp; even the
  I/O-free EquatorialSystem getter blipped). Three consecutive runs each had
  exactly one such blip until the governor was pinned to `performance` --
  then 0 timing violations. Same class as the RK3568's `interactive`
  governor (that image got `openastro-cpufreq.service`); consider the same
  for the CM4 image. Not Wi-Fi: power save was off and the BSSID pinned.
- **Wi-Fi UDP field lessons** (Wave AP + SBC): (1) a single-radio SBC running hotspot
  (`ap0`) + client (`wlan0`) dual-role flaps the link — disable the hotspot while the
  mount Wi-Fi is in use (and beware hotspot subnets clashing with the mount's
  192.168.4.x); (2) an AP rejoin can change the local address, invalidating a
  `connect()`ed datagram socket (`ENETUNREACH`) — the wrapper rebuilds the socket and
  resends once; (3) retransmit duplicates cause reply mis-pairing — defenses are
  drain-before-send, a settle drain after any timeout, and per-command expected reply
  length validation; (4) run ConformU on the SBC itself (localhost), not across the LAN —
  VM-to-SBC jitter alone produces FAST-target (0.1 s) violations.
- **Disconnect all stray Alpaca clients before a ConformU run**: the per-client Connected
  registry keeps the device physically connected for other ClientIDs, so leftover test
  sessions carry state (targets, tracking) into ConformU's "first time use" checks.
- Deploy note: the systemd service executes `/usr/bin/alpacabridge` — install the built
  `alpacahttp_server` there (NOT `/usr/local/bin/`), and verify with
  `md5sum /usr/bin/alpacabridge` after restart; a wedged park/slew thread can hang
  `systemctl stop` (use `systemctl kill -s SIGKILL`).

#### EQ-class Synta boards (EQM-35 Pro and relatives) — 2026-09-06

The `:` command set is identical on classic Synta EQ mounts, so the Wave driver drives
them unchanged. What differs is the transport and the identity, and both bit us:

- **Baud is NOT irrelevant off the Wave.** The Wave's USB port is STM32 CDC-ACM, where
  the baud setting is ignored. Synta EQ boards reached over the mount's own USB port or
  an EQDIR cable are real UART bridges: the **EQM-35 Pro's built-in port is a soldered
  Prolific PL2303 (067b:23a3, "ATEN Serial Bridge") at 115200**, and a 9600-only scan
  finds nothing at all. Enumeration probes 9600 then 115200; the probe's winning baud
  MUST be carried into `ConnectionInfo` (auto-detect used to drop it, so a board found
  at 115200 was reopened at 9600 and every command timed out).
- **`":e"` byte 3 is the MOUNT CODE, not a firmware patch level.** Layout is
  `<fw major><fw minor><mount code>`, matching INDI `skywatcherAPI.cpp`. The Wave's
  `=033A44` is firmware 3.58 + code 0x44 (WAVE_100I), never "3.58.68". EQM-35 Pro:
  `=032732` -> firmware 3.39, code **0x32**, a code in neither INDI's `MountType` enum
  nor Sky-Watcher's published SynScan model list. Cross-confirmed: the SynScan handset
  on the same mount reports model id 50 (= 0x32) from its own `m` command, so
  `synscan_model_id_to_name` gained `case 50` too.
- **Feature word tells you which mount you are on.** `":q"` with data 0x000001 succeeds
  on EQ boards — it does not throw — the home-index bit is simply absent. EQM-35 Pro
  returns **0x7000** (POLAR_LED | COMMON_SLEW_START | HALF_CURRENT_TRACKING); the Wave
  returns 0x100C (POLAR_LED | IS_AZEQ | HOME_INDEXER). Flags follow EQMod's set. Gate
  AutoHome on the 0x04 bit, never on `":q"` failing: an EQM-35 takes the count-frame
  `FindHome` fallback, and running the sensor hunt on a mount with no index sensors
  would drive the axes looking for an edge that never arrives.
- Both presets live in `FakeSkyWatcherMount` as `FakeMountProfile::wave_100i()` /
  `eqm35_pro()`, so loopback tests run against real captured geometry.
- **Hardware bring-up, EQM-35 Pro over the mount's built-in USB, 2026-09-06** (Raspberry
  Pi 3B, Debian 13 arm64, direct USB-A-to-B, no handset in the chain): auto-detect found
  it unaided -- `Found Sky-Watcher EQM-35 Pro on /dev/ttyUSB0 (MC firmware 3.39, 115200
  baud)` -- and `Name` reported "Sky-Watcher EQM-35 Pro", firmware "3.39". CCDciel
  connected over Alpaca with zero driver warnings. Further bring-up notes (pointing math,
  MoveAxis semantics, tracking-rate measurement, the southern-hemisphere fixes) are
  recorded against those fixes elsewhere in this section.
- **SynScan hand controller in "PC Direct Mode" reaches this driver unchanged, 2026-09-10**
  (open-astro#275; EQM-35 Pro, SynScan V4 handset, Raspberry Pi 3B). The handset's menu
  setting switches its own USB port from the SynScan command set to the raw motor-controller
  protocol, so a `skywatcher` serial device pointed at the *handset's* port (9600 baud — PC
  Direct Mode keeps the PC-facing rate) connects exactly like the board's own port: identity
  `EQM-35 Pro (mount code 50), firmware 3.39`, Declination bit-identical to the direct port
  (so `:a`/`:b`/`:g` geometry relays intact), `PulseGuide` N/S +11.25" and back to the same
  count. The #242 echo guard steps aside by itself — a handset in this mode no longer answers
  the SynScan echo — and the SynScan driver's scan then finds nothing on that port, which is
  correct. Caveat: command latency through the 9600-baud relay is higher and more variable
  than the board's own port; open-loop `MoveAxis` legs of ±2 deg/s for 1 s netted ~7 arcmin
  instead of ~2 arcsec. Driver-timed motion is unaffected. Prefer the mount's own USB port
  or an EQDIR cable where available; PC Direct Mode is a working no-extra-hardware fallback
  for classic mounts that have neither (the #230 audience). Docs line for
  `SUPPORTED-DRIVERS.md` lands with the post-ConformU direct-driver docs PR.

#### Goto landing, tracking restart and the dev-VM clock (EQM-35 Pro) — 2026-09-12

Full ConformU on the EQM-35 Pro over USB, in a Lima Debian 13 arm64 VM on an Apple Silicon Mac
with the mount's USB-serial bridge passed through by VirtualHere. Five full runs; each finding
below was one of them.

- **The controller's stopped flag is not the end of a goto.** After a 6 h slew and three landing
  refinements, the last refinement's landing read 11 counts short of its `:S` target while `:f`
  already said stopped; every clean landing in the same log read exactly on target. The tracking
  restart (`:K1 :G111 :I1 :J1`, sidereal period written and read back correctly) sent 7 ms later
  left the RA axis running at ~2x sidereal for the rest of the session (raw `:j1`: 1251 counts in
  the 6 s of a Dec-only pulse, 9716 counts in the 47 s to FindHome). ConformU saw it as
  `PulseGuide +9.0 North` "East-West movement outside tolerance, RA change -5.68 s". Not
  reproducible on demand (five targeted attempts incl. the identical slew shape). Driver now: a
  slew is complete only when the axis reads stopped AND two `:j` reads 60 ms apart agree
  (`wait_axis_stationary_locked`, also between refinement gotos); `Slewing` stays true until
  tracking is restarted; the restart is rate-checked over 300 ms and redone once with a WARN
  ("Post-slew tracking restart: RA axis running at N counts/s") if off by >25%. The check is
  skipped, with an INFO naming the count, when the window cannot accumulate 4 counts: ":j" is
  whole counts and both reads truncate, so below that every possible reading lands outside the
  tolerance and the check would condemn a healthy axis (an effective RA rate near zero, e.g.
  RightAscensionRate ~0.9 nearly cancelling sidereal, is the way in). Grep for that WARN if a
  2x ever recurs, and for "rate check skipped" if a slew was never verified: **every exit that
  does not complete a measurement logs that phrase** -- the entry guards, the zero-rate and
  zero-interval guards, both `sleep_unlocked()` supersession exits, the three exits inside the
  attempt-0 recovery (the stop-wait losing the axis, tracking going off while it settled, and the
  restart itself throwing), and the two catch blocks (a position read throwing mid-window, and
  the caller's catch around the whole check, which fires when `stop_axis_and_wait_locked()`
  throws inside the recovery; a `check_connected()` throw out of the sample sleep lands in the
  position-read catch). Review of this branch found three of those silent, including one that fires
  with the RA axis already stopped by the check's own stop, and a second review found the three
  exception paths silent too. A check that RAN and found the rate correct logs
  nothing -- that is the ordinary case, once per goto, and the grep is for slews that were
  never verified, not for slews that passed. Power was a suspect (mount fed from an SVBONY SV241's 12 V rail; the
  event followed a 26 s full-speed slew) but was not proven.
- **A retry loop whose supersession test compares against a generation captured before the loop
  can only ever run once.** Review of the branch above: the rate check's second sample was
  unreachable, because the attempt-0 recovery is itself a motion command (its own
  `++motion_generation_`, and `start_speed_motion_locked()` bumps it again), so attempt 1's first
  re-lock read the check's OWN restart as another command's supersession and returned. The
  "restart did not correct it" WARN could never be emitted, and a restart that also latched wrong
  ran at the wrong rate in silence -- the exact failure the check exists to surface. Fixed by
  re-seeding the entry generation from the recovery's own restart. **Rule:** whenever a loop both
  issues a motion command and guards itself with "has the generation moved", the guard's baseline
  has to be re-established after each of the loop's own commands, or every iteration after the
  first is dead code. The tell is a `const` generation captured outside the loop. Pinned by a
  case arming two bad latches instead of one (`restart_tracking_at_wrong_rate(1, 2)`) and
  asserting the second-attempt WARN.
- **One flag cannot answer two questions, and "Slewing" is not "the axes are busy".** Holding
  `goto_in_progress_` across the post-slew restore was the right fix for the Slewing half (a
  client must not fire motion into the restart window) and a regression for the other: that
  same flag feeds `axes_busy_locked()`, which the rate setters read as "a goto owns the axes,
  its restore will re-apply this when it releases them". The restore had already run. A
  `RightAscensionRate` write landing in the window returned 200, read back the new value, and
  was never driven. Fixed with a second flag, `restoring_tracking_`, that
  `get_slewing_locked()` consults and `axes_busy_locked()` does not. **Rule:** before widening
  the span of a state flag, list every predicate that reads it and check each one still wants
  the wider span. Here `get_slewing_locked()` did, `axes_busy_locked()` did not, and the
  duty-cycle worker's start gate did -- so it names the new flag explicitly, because a burst
  in that window would bump `motion_generation_` under the rate check's supersession guard and
  make it skip. Same family as the per-axis `axes_busy_locked()` finding in #432: a predicate
  that bundles several questions eventually gets asked the one it answers wrongly.
- **A measured estimate needs a test that the estimate MOVES, not that it helps.** The
  constants-to-EMA change (`goto_overhead_seconds_`, `resume_latency_seconds_`) shipped with
  nothing pinning it: delete both update blocks, re-seed from the constants, suite still green.
  The estimates are private, so the observable is the thing they steer -- the RA landing
  residual, which is pure aim-ahead error since Dec has no time term and lands exactly on
  target every slew. Over five identical slews the residual spread is ~9.3 arcsec measured and
  ~0.13 arcsec frozen, stable to +/-0.1 across runs; the case asserts a 2 arcsec floor.
  **And the seam disagrees with the hardware about which is better**: on the loopback fake the
  frozen constants land at about -0.8 arcsec and the measured EMAs at -3 to -13, because the
  fake has no equivalent of the real MC's ~3 s floor on even a 350-count refinement goto --
  which is precisely the fact that made the constants wrong on an EQM-35. So the test pins that
  the aim-ahead is driven by something that moves, and says in its own comment that the
  evidence measuring HELPS is the hardware ConformU run, not the fake. **Rule:** when a fake
  cannot reproduce the quantity a change was made for, pin the mechanism and name the real
  evidence in the test, rather than asserting an improvement the fake will contradict.
- **Test seams have to model the failure, not a nearby one.** The landing-settle wait
  (`wait_axis_stationary_locked`) shipped with nothing in the suite failing without it, and the
  ramped-`:K` seam that looked like it should cover it could not: a ramped stop keeps `:f`
  RUNNING for the whole ramp, which the ordinary stop-wait already handles, so the stationary
  check had no window left to close. The real window is the one the hardware showed -- `:f`
  clearing while the last counts still arrive -- and it needed its own seam (`land_short_by()`:
  report the landing stopped N counts short, then creep the remainder in). Goto counts could not
  be the signal either (`refine_goto_landing()` burns all three iterations on this fake whether or
  not a landing coasts), nor wall-clock timing (the 3 s `slew_force_until_` window and the
  tracking restore both sit between the landing and `Slewing` clearing). What works: coast for
  longer than `kLandingSettleTimeout` and assert the check's own give-up WARN, a string nothing
  else emits. **Rule:** before claiming a change is covered, delete it and run the suite; if it
  stays green, the seam models the wrong failure.
- **Goto aim-ahead constants are rig-specific: measure them.** `kGotoRampSeconds` (2.5 s) and
  `kTrackingResumeSeconds` (0.7 s) were tuned on the Wave 100i. On the EQM-35 the landing-to-`:J1`
  restart takes ~0.2 s and even a 350-count refinement goto ~3.1 s (the MC's minimum goto time),
  so 2.5 + 0.7 happened to equal 3.1 + 0.2 for refinements (which is why they landed to 0.6 arcsec)
  while a 20 deg goto whose estimate ran 1.2 s long read as 6 arcsec off at the deadband check and
  resumed tracking 1.03 s ahead of the sky (`SyncToCoordinates` "15.4 arc seconds away", exactly
  1.03 s of RA). Fixing only the restart latency made every refinement land 0.6 s late
  (`SlewToCoordinates` "10.8 arc seconds away"). Both are now EMAs measured per goto
  (`goto_overhead_seconds_`, `resume_latency_seconds_`), seeded from the constants so the first
  goto of a session is unchanged on every mount; the refinement loop remains the safety net.
- **Run chrony on the machine running ConformU. A stepped clock is an RA error.** RA = LST - HA
  with LST from the host clock. Lima's host agent steps the guest clock by ~100 ms whenever the
  drift passes its threshold (every 2-3 min at the ~500 ppm a vz guest drifts; no knob in Lima
  2.2.0), and `systemd-timesyncd` does not correct frequency. Every 10 s rate-offset measurement
  or Dec pulse that spans a step fails by exactly 0.1 s of RA: `RightAscensionRate Write`
  -0.0136 vs -0.0033 s/s (twice, at different hour angles), `PulseGuide +3.0 South` 0.10 s
  east-west; the raw RA counts were exactly sidereal both times and the step timestamps in
  `~/.lima/<vm>/ha.stderr.log` ("guest clock adjusted") sat inside each measurement window.
  `apt install chrony` (fast poll: `minpoll 3 maxpoll 5`, one `chronyc makestep`) holds the drift
  at ~65 ms with no steps; the passing run had none. `/conformu` Step 2f2 now requires chrony.
- **ConformU's `-9.0 / +9.0 / -3.0 / +3.0` test labels are hour angles.** The extended
  rate-offset and pulse-guide tests slew to HA -9, +9, -3 and +3 h and repeat each measurement
  there; a failure at one label and not another is position/timing-dependent, not a sign flip.
- **VirtualHere for the USB pass-through** (Lima vz has none): the free server refuses `USE`
  from a client started with `-n` ("running as a service"); run `vhclientarm64` without `-n`.
  The client needs `vhci-hcd`, which Debian's `cloud` kernel lacks -- install `linux-image-arm64`.

#### Alignment with upstream issue #230 (EQMOD-style direct motor-controller support)

open-astro/AlpacaBridge#230, filed by the maintainer, asks for exactly the work in this
section: generalizing the Wave driver to classic Sky-Watcher/Orion EQ mounts (HEQ5, EQ6,
EQ6-R, AZ-EQ6, EQ5 Pro, etc.) via EQDIR cable, with no hand controller in the loop. Status
against its checklist, 2026-09-06:

- [x] Model/feature detection via `:e`/`:q` — done (mount-code table, feature-word gating).
- [ ] Board-capability gating for PPEC, dual-encoder, WiFi, and the polar-scope LED per the
  issue's list — only the home-index bit (`0x04`) is actually consulted so far.
- [x] CPR/high-speed-ratio/timer-freq read from the board, not hardcoded for Wave —
  confirmed: EQM-35 Pro geometry (CPR 9,216,000, timer 16 MHz) differs from the Wave
  (4,147,200 / 14 MHz) and the SAME driver code tracked correctly on it (0.99995x
  sidereal), so this was already correct, just unverified until now.
- [x] High/low speed mode switch threshold — already board-generic:
  `kFastModeThresholdDegPerSec = 128.0 * kSiderealDegPerSec`, derived from the MC
  protocol's universal 128x switchover, not a Wave-specific constant.
- [x] AutoHome/FindHome gracefully disabled without home-index sensors — hardware
  verified: the EQM-35's `0x7000` feature word has no `HOME_INDEXER` bit, and `FindHome`
  correctly takes the count-frame goto fallback rather than hunting a sensor that
  doesn't exist.
- [x] Naming/config: model auto-detected under the existing `vendor: skywatcher` key
  (no separate `eqmod` alias needed) — done, `get_name()` reports the real model.
- [x] **Auto-detect distinguishing an EQDIR cable from other vendors' PL2303/CH340/FTDI
  devices** — was a real gap: the enumeration scan and `connect_serial()` did not use
  `alpacacore/util/serial_port_registry.h` (the cross-vendor in-use registry originally
  built for WandererAstro, explicitly designed to generalize "across wrappers"). Fixed:
  both scan loops skip a port another connected device holds open, `probe_skywatcher_port`
  re-checks after `open()` for the TOCTOU window, and `connect_serial()` claims the port
  in the registry BEFORE opening it and releases it in `disconnect_locked()`. The gap is
  wider than this vendor: only WandererAstro (all four wrappers) and Gemini's PDH wrapper
  (`gemini_pdh_protocol_wrapper.cpp`) use the registry; synscan, ioptron, celestron, onstep
  and Gemini's focuser/flat-panel wrappers do not — only `skywatcher` was closed here, in
  scope for this issue. The claim/release in `connect_serial()` is covered by a pty-backed
  test in `test_skywatcher_serial.cpp`; the post-`open()` re-check in `probe_skywatcher_port`
  narrows the TOCTOU window but cannot close it (in-process best-effort set, not a file lock).
- [ ] Pier side / meridian handling for GEMs in the southern hemisphere — open-astro#261.
  Audit (2026-09-09, no hardware): unlike the RA/Dec direction bugs above, the branch that
  drives `SideOfPier`/`DestinationSideOfPier` is chosen purely from the sign of hour angle
  in `ra_dec_to_axis_degrees_locked()`. Since #432 that function consults
  `hemisphere_south_locked()` twice -- `sky_sign` multiplies both `dec_mech` (the a2
  magnitude) and the `a1` term -- but still never for which branch is picked or which
  side it is labelled, which is the half this audit rests on.
  So the reported side already satisfies the ASCOM flip-with-HA contract (the same one the
  OnStep driver had to learn the hard way, see below) in both hemispheres by construction, and
  a loopback or ConformU check can only confirm that self-consistency — it cannot tell whether
  the "pierEast" branch is the true physical east side below the equator, because there is no
  internal contradiction to expose (whichever side the code calls pierEast, it consistently
  slews to and reports that side). Loopback regressions asserting the flip contract on the
  EQM-35 Pro and Wave profiles are in `test_skywatcher_async.cpp` ("Pier side across the
  meridian"). The physical-side question stays open until the plate-solved goto-across-the-
  meridian check on the rig (see the hemisphere fixes and pending bench test elsewhere in this
  section).
- [ ] `SyncToCoordinates` single-point offset sync model — not exercised this session
  (no plate solve performed).
- [ ] Park/unpark weights-down convention — not specifically re-verified on a classic
  board this session (uses the same `kHomeCounts` convention as the Wave; untested here).
- [ ] ConformU 4.5.x on a classic mount — blocked on Pi 5 hardware availability; not the
  EQM-35 specifically, but the issue's ask applies equally.
- [x] Fake mount test double extended with a classic-board profile: `FakeMountProfile::eqm35_pro()`
  in `AlpacaCore/tests/fake_skywatcher_mount.h` is a REAL EQM-35 Pro capture (its built-in PL2303
  port answers only at 115200), used by the `[eqm35]`-tagged cases in
  `AlpacaCore/tests/test_skywatcher_async.cpp`: identity from the mount code, the count-frame
  FindHome fallback, the board's own sidereal period, and the `[hemisphere]` southern-hemisphere
  regressions that depend on its geometry and feature word (one `[eqm35]` case uses the Wave
  profile as the control). Counts are deliberately not stated here; grep the tag.
- [ ] A second classic-board profile (HEQ5 PRO / EQ6, 9600 baud over an EQDIR cable, older
  firmware string) — deliberately NOT added with invented numbers: fabricating a plausible
  profile without hardware to source it from would misrepresent guessed values as measured
  ones. HEQ5 PRO and EQ6 hardware is on hand via the `synscan` (hand-controller) driver
  validation (#7, #29); capture an actual reading from it over an EQDIR cable when available.
- **Not yet done, intentionally: adding the EQM-35 Pro to `SUPPORTED-DRIVERS.md` and the
  architecture table, and renaming the "Sky-Watcher Wave" section to "Sky-Watcher Direct
  Motor Controller" per the issue's suggestion.** This PR's code and tests are ready for
  review now; the "supported"/Production claim is deliberately withheld until a ConformU
  pass is run on the EQM-35 Pro (blocked on Pi 5 hardware, per this repo's own documented
  bar in `AlpacaCore/conformu/README.md` and `SUPPORTED-DRIVERS.md`). A follow-up
  docs-only PR adds those lines once that report exists.
#### KNOWN BUG (FIXED): superseded MoveAxis stop task strands `Slewing` and kills tracking

Found on an EQM-35 Pro 2026-09-06, but **not hemisphere- or model-specific — the Wave
100i is equally affected.** Not caused by the southern-hemisphere RA fix; that change
only altered a rate sign and does not touch this machinery.

**Symptom.** After a sequence of `MoveAxis` presses, the driver reports `Slewing = true`
indefinitely while the axis is demonstrably stopped (`":f1"` running bit clear, `":j1"`
counts frozen), AND tracking is never restarted even though `Tracking` still reports
true. The mount sits motionless claiming to be both tracking and slewing. Reported RA
then drifts at 1.0x sidereal — the signature of a stationary mount — instead of holding.

This is the dangerous shape: a sequencer that waits for `Slewing` to clear before
exposing hangs forever, and one that does not wait images on an untracked mount.

**Mechanism.** `move_axis()` uses a SINGLE shared `stop_task_thread_` for both axes.
When a new stop supersedes a pending one, the old task is cancelled
(`stop_task_cancel_.store(true)`) and returns early from `task_wait_for()` — before
reaching `manual_axis_slewing_[axis] = false` and the restore-tracking tail. Its axis's
flag is stranded set, and `get_hardware_slewing_locked()` returns true forever because
it ORs both `manual_axis_slewing_` entries.

**Reproduction.** Drive MoveAxis on alternating axes with stops close together — CCDciel
issues MoveAxis pairs ~44 ms apart on button release (observed in the journal), which is
enough for the second stop to cancel the first axis's task. N, S, E, W in sequence
reproduced it reliably.

**Recovery (user-level).** `PUT moveaxis Axis=<n> Rate=0` on the stranded axis clears the
flag and restores tracking, because a stop on an axis whose flag is set spawns a fresh
task that runs to completion.

**Fix (done).** `stop_task_thread_` and `stop_task_cancel_` are now per-axis arrays;
`reap_stop_task(axis)` and the spawn/retry-join block only ever race with a prior task
for the SAME axis. A new loopback regression reproduces the exact scenario (RA stop
dispatched, Dec stop dispatched while RA's stop task is still mid-ramp) and asserts
`Slewing` clears promptly. The generation guard
(`motion_generation_ == stop_task_generation`) is unchanged and still gates the
tracking-restore tail — which is exactly what exposed the SECOND bug below.

#### KNOWN BUG (FIXED): cross-axis `motion_generation_` can block a same-axis tracking restore

Found while writing the regression test for the bug above, on the SAME night
(2026-09-06) — the per-axis stop-task fix is necessary but not sufficient. Not
hemisphere- or model-specific.

**Symptom.** With the per-axis fix in place, `Slewing` now clears correctly after
stopping both axes close together — but the RA axis can still fail to resume tracking
after a `MoveAxis(0, 0)` stop, even though `Tracking` reports true throughout. Caught by
a loopback test asserting `mount.axis_running(1)` becomes true again after the stop
settles: it does not, reliably, when a Dec-axis stop is dispatched while the RA stop
task is still polling.

**Mechanism.** The restore-tracking tail guards itself with
`motion_generation_ == stop_task_generation` — "only restore if nothing newer
superseded this stop." But `motion_generation_` is ONE counter bumped by every motion
command on EITHER axis (see its declaration: "bumped by every motion command"). A Dec
stop dispatched while RA's stop task is polling bumps the shared counter for a reason
that has nothing to do with RA, so the RA task's tail reads a mismatch and silently
skips restoring RA's tracking — even though nothing actually superseded the RA stop
itself (which is correctly detected via the now-per-axis `stop_task_cancel_[0]`, a
separate and correctly-scoped check).

The codebase already has the right idiom for this elsewhere: the duty-cycle worker
(`apply_ra_drive_locked`'s burst path, guarding sub-floor rate duty-cycling) computes a
`same_axis_owner` flag from `goto_in_progress_ || parking_ || homing_ || slewing_cached_
|| manual_axis_slewing_[i] || (pulse_guiding_active_ && pulse_axis_ == channel)` before
trusting a generation mismatch as a real supersession, specifically BECAUSE "the global
generation cannot tell a same-axis supersession from an unrelated other-axis command"
(exact wording from that code's own comment). The MoveAxis stop-task tail does not apply
this idiom and should.

**Fix (done).** Applied option (a): the stop-task restore tail now computes a
channel-scoped `same_axis_owner` (`goto_in_progress_ || parking_ || homing_ ||
slewing_cached_ || manual_axis_slewing_[axis] || (pulse_guiding_active_ &&
pulse_axis_ == channel)`), the exact idiom the duty-cycle worker already uses, and only
treats a `motion_generation_` mismatch as a real supersession when `same_axis_owner`
is true. Verified the historical regression this guards against (PR #216 round-5:
`SetTracking(false)` racing the restore) is still covered independently: that path sets
`tracking_ = false` under the SAME `mutex_` this task also holds, so there is no
interleaving where the restore reads `tracking_ == true` while a completed
`SetTracking(false)` meant otherwise -- the `tracking_ &&`/`dec_rate_arcsec_per_sec_ !=
0.0 &&` guards already ahead of the generation check cover that case on their own.
Extended the regression test from the first bug to assert the RA axis actually resumes
running (not just that `Slewing` clears); confirmed it fails at exactly that assertion
with the fix reverted to the raw equality check, and passes with it restored.

#### KNOWN BUG (FIXED): `axes_busy_locked()` is the wrong question for a per-axis re-apply

Found in review of #432 (2026-09-12), the third instance of the same idiom in this
driver. Not hemisphere-specific in shape, only in trigger.

**Symptom.** `SiteLatitude` written across the equator while a North/South pulse or a
`MoveAxis` on the DECLINATION axis is in flight leaves the RA axis running the old
hemisphere's direction indefinitely -- stars trail at 2x, the #250 signature, with
nothing scheduled to correct it. Under an autoguider this is the common case, not the
corner: roughly half of a session's corrections are declination, and PHD2 holds
`pulse_guiding_active_` true for most of every guide cycle.

**Mechanism.** A setter that must re-command an axis skips when the axis is busy, on the
grounds that the busy operation's own restore path re-derives the value. That contract
holds per axis, but the guard asked `axes_busy_locked()`, which is true when EITHER axis
is busy. A declination pulse ends in `stop_axis()`'s final `else` branch -- it stops the
DEC axis and re-applies the DEC offset, and never touches RA. The `MoveAxis` stop task is
the same shape: its restore calls `set_tracking_locked()` only for `channel == kAxisRa`
and `apply_dec_rate_offset_locked()` only for `kAxisDec`. So both the setter and the
in-flight operation skipped RA, each expecting the other to do it.

**Fix (done).** `axis_busy_locked(channel)` is now the primitive -- the same
`goto_in_progress_ || parking_ || homing_ || slewing_cached_ || manual_axis_slewing_[i]
|| (pulse_guiding_active_ && pulse_axis_ == channel)` idiom the duty-cycle worker and the
MoveAxis stop tail already use -- and `axes_busy_locked()` is defined as the OR of the
two, so every existing caller is unchanged. `set_site_latitude()` decides each half
separately: re-apply the RA drive unless RA is busy, re-apply the Dec offset unless Dec
is busy, and skip entirely only when both are. A sub-floor RA rate is pre-armed into
`ra_duty_rate_deg_s_` when RA is busy, the way `set_right_ascension_rate()`'s busy branch
already does, because the duty worker resumes from that stored rate and no restore path
re-derives it. Two loopback regressions (`test_skywatcher_async.cpp`) drive the
declination-pulse and declination-`MoveAxis` variants and were confirmed to fail on the
pre-fix setter with the RA axis still counting the old way.

**Rule for the next driver.** Any "skip while busy, the restore path will re-apply"
guard has to name the axis it is talking about, and the reviewer's question is always:
does the operation that owns the busy axis actually re-derive THIS value? A mount-wide
busy flag can only answer that when the operation owns every axis -- a goto, park, home
or slew does; a pulse or a manual nudge does not.

The audit found two more instances, both fixed with it, and it took three review rounds
to find all three -- each fix's own claim of completeness was what exposed the next one.
No hemisphere is involved in either:

- `set_right_ascension_rate()`: a declination operation in flight made the whole-mount
  predicate true and stranded the rate write with nothing scheduled to apply it, so a
  client's `RightAscensionRate` silently did nothing until the next re-apply.
- `set_declination_rate()`, which passes the predicate down as
  `apply_dec_rate_offset_locked(defer_motion=)`: an East/West pulse or an RA `MoveAxis`
  made it true while owning only the RA axis, and the pulse's `stop_axis()` restore
  rewrites the RA step period without ever calling `apply_dec_rate_offset_locked()`, so a
  continuous Dec offset was stranded the same way. Comet or satellite tracking while
  autoguiding is the way in. Only the continuous branch was affected: a sub-floor rate
  recovers on its own through the duty worker's start gate.

Every "skip while busy" guard in this driver now names its axis. When adding a new one,
grep for `axes_busy_locked()` and justify each remaining caller: the legitimate uses are
the ones asking "is the mount doing anything at all", such as the duty worker's
`connected_ && tracking_ && !axes_busy_locked()` start gate. **And do the grep before
writing the claim** -- this section asserted the sweep was complete twice before it was,
and each time the assertion itself was the review finding.

- **Hardware bring-up, EQM-35 Pro over the mount's built-in USB, 2026-09-06** (Raspberry
  Pi 3B, Debian 13 arm64, direct USB-A-to-B, no handset in the chain):
  - Pointing math at latitude -37.2: home points at the SOUTH celestial pole, so
    `dec = -90 + a2`. The session "verified" this against raw counts — reported HA
    matched axis 1 to 0.0004 deg, and alt/az recomputed from the reported RA/Dec matched
    to 4 decimal places. **That was self-consistency only**: the driver reports the model
    it commands, so those checks could not see that the RA-axis/hour-angle relation was
    missing its 6 h home offset and its southern sign (#432, found from a Wave 150i sky
    test in the north). The dec relation was right; the RA relation is now
    `HA = -(a1/15) ± 6` here. Treat every "reported coordinates matched" line in this
    section as a consistency check, not a sky check.
  - `MoveAxis` verified semantically in all four directions, not just for motion:
    each button was checked against the change in REPORTED RA/Dec. N: Dec +15.59
    deg, S: Dec -16.96 deg, E: RA +15.47 deg, W: RA -15.28 deg, zero cross-axis
    coupling in every case. **The two RA rows are stale as reported values**: they were
    read under the pre-#432 model, where `d(HA)/d(a1)` did not flip below the equator.
    The same mechanical button now moves reported RA the other way at this site. What
    the rows still establish is the mechanical fact, which way each button turns which
    axis; only the RA/Dec labels on them changed. `move_axis()` applies NO branch or hemisphere sign
    transform (the rate goes straight to `start_speed_motion_locked`), so this is
    also the hardware reference for which way a raw Dec-axis rate moves reported
    Dec below the equator -- the fact the DeclinationRate/PulseGuide fix below
    rests on. Reported coordinates come from the driver's own pointing model; an
    independent sky check (plate solve) is still on the list below. Do NOT "fix"
    MoveAxis to follow sky Dec: the ASCOM spec says the sign of the Rate parameter
    "is purposely left undefined" and the motion is about the MECHANICAL axis, so
    the no-transform behaviour is correct in both hemispheres (checked against
    ascom-standards.org/newdocs/telescope.html#Telescope.MoveAxis, 2026-09-06).
  - **Tracking rate measured at 0.99995x sidereal over 5 minutes** (-46 ppm,
    -2.5 arcsec/hour, against a +/-31 ppm encoder-quantisation floor), Dec drift
    exactly 0 counts. Ten consecutive 30 s intervals of -3214 counts, +/-1. The
    magnitude stands; the DIRECTION that session settled on (counts up, #250) was
    reversed by #432: below the equator the counts must go down, which is what the
    original code did and what indi-eqmod does. The "2.007x sidereal" that condemned
    it was the reported RA of the old model, not the sky.
  - **Technique worth reusing:** the protocol wrapper does NOT log individual
    commands, so do not plan to read step periods out of the journal. Sample
    `":j1"`/`":j2"` through the Alpaca `commandstring` passthrough instead and
    differentiate — that measures what the mount ACTUALLY does rather than what it
    was told, and needs no rebuild. Expected sidereal counts/s = `CPR * 360.98564736629
    / 86400 / 360` (106.959 on this mount). Make sure nothing else is driving the
    mount while sampling; a manual slew mid-run silently corrupts the result.
  - Note this validates driver -> board -> encoder counts. It validates counts -> SKY
    only if the gear ratio matches what the firmware's `":a"` assumes; a belt/pulley
    mod that changes the reduction would track perfectly in counts and still drift on
    sky. (Confirmed ratio-preserving on this unit.)
  - **Dec-axis direction of `DeclinationRate` / `PulseGuide` North-South below the
    equator: MEASURED on the mount 2026-09-06** (build `d29d650`, Pi-native arm64 build,
    daylight, OTA mounted). Method: `Connected=true`, tracking on at sidereal, Dec axis
    first offset +0.51 deg from home with `MoveAxis` so a2 > 0 -- **do not run this test
    from the home position: at a2 = 0 (Dec -90) reported Dec rises for EITHER mechanical
    direction, so the pass/fail signature is invisible there.** Reported Dec and raw
    `":j2"` counts (via `commandstring`) sampled around each command:
    PulseGuide North 5000 ms -> Dec +37.7" / +268 counts (expected +37.6" / +267 at the
    default 0.5x sidereal guide rate); South -> -37.7" / -268, net 0.
    `DeclinationRate` +5"/s for 60 s -> +302.9" / +2159 counts (expected +300" / +2133,
    the excess is the ~60.7 s wall time); -5"/s -> -299.7" / -2144; rate 0 -> 0 counts of
    drift in 30 s. Both call sites of the KNOWN BUG fix below are confirmed on the a2 > 0
    branch; the a2 < 0 branch rests on the loopback tests only -- see the PENDING BENCH
    TEST below, which reaches it WITHOUT a real meridian flip. Same session: reported RA
    held constant to 1e-5 h over ~90 s of tracking (a self-consistency result only: the
    "RA tracking-direction fix" it was read as confirming is the #250 removal that #432
    reversed),
    and `MoveAxis(Dec, +rate)` again moved reported Dec and the counts up. Mount returned
    to home, tracking off.
  - STILL UNVALIDATED on EQ-class hardware: absolute pointing (needs a plate solve and
    sync), `SideOfPier` and meridian-flip behaviour in the southern hemisphere, the
    `":g"` high-speed ratio under fast slews, and the Dec-axis direction of
    `DeclinationRate` / `PulseGuide` North-South below the equator (fixed in code from
    the pointing model -- see the KNOWN BUG below -- but not yet measured on the mount;
    a short autoguiding session is the cheapest check).
  - **PENDING BENCH TEST (not yet run): `a2 < 0` Dec-direction sign coverage.** Closes the
    gap above. Key realization (2026-09-07): `a2` is the raw Dec-axis angle relative to
    home (see `compute_ra_dec_locked()`) and is NOT coupled to the RA axis at all, so the
    `a2 < 0` branch does not require an actual GOTO across the meridian -- the same
    `MoveAxis` bench technique already used for `a2 > 0` reaches it directly, mirrored:
    1. `Connected=true`, tracking on at sidereal, OTA mounted, daylight is fine (same
       setup as the `a2 > 0` session, 2026-09-06).
    2. `MoveAxis` the Dec axis to roughly **-0.5 deg from home** (the OPPOSITE direction
       from the `a2 > 0` session's +0.51 deg) so `a2 < 0`. Do NOT start from `a2 = 0`
       (Dec -90): reported Dec rises for either mechanical direction there, so the
       pass/fail signature is invisible right at home -- same caveat as the `a2 > 0` run.
    3. Sample reported `Declination` and raw `":j2"` counts via the `commandstring`
       passthrough around each command (same technique as the `a2 > 0` bring-up notes).
    4. `PulseGuide` North 5000 ms -> expect reported Dec to RISE; South -> back to
       baseline, net 0 counts.
    5. `DeclinationRate` +5"/s for ~60 s -> expect Dec rising roughly 300" (accounting for
       actual wall time as in the `a2 > 0` run); -5"/s -> back down; rate 0 -> no drift in
       30 s.
    6. Compare signs against the fix's table: on the `a2 < 0` (west) branch, southern
       sites should NEGATE (previously wrongly kept) and northern sites should KEEP
       (unchanged) -- the mirror image of the `a2 > 0` row already confirmed.
    This closes ONLY the sign-rule coverage gap. It does NOT validate `SideOfPier`
    reporting or automatic pier-flip behaviour during a real GOTO across the meridian --
    that is the separate, still-open bullet directly above, and realistically waits on
    the plate-solve work since confirming a flip landed correctly needs an independent
    sky check.

#### KNOWN BUG (FIXED): DeclinationRate and PulseGuide North/South run backwards south of the equator

Found by static review on 2026-09-06 while auditing the hemisphere-conditional code
after the RA tracking-direction fix above -- NOT on hardware. Not model-specific: any
Sky-Watcher mount on this driver at a southern site was affected; northern sites never
were.

**Symptom.** Below the equator, a positive `DeclinationRate` drives the reported
Declination DOWN, and a `PulseGuide` North pushes the star further south. For an
autoguider this is the dangerous shape: every Dec correction lands on the wrong side, so
the guide loop diverges instead of converging. Magnitudes were always right, only the
direction was wrong -- exactly the signature of the RA bug above, which is why a
rate-only check never caught it.

**Mechanism.** Both `apply_dec_rate_offset_locked()` and the North/South branch of
`pulse_guide()` chose the axis direction with the plain rule "a2 >= 0 -> negate the
rate", derived from the northern pointing formula `dec = 90 - a2` (so d(dec)/d(a2) = -1
on that branch). But `compute_ra_dec_locked()` mirrors Dec below the equator
(`dec_sky = -(90 - a2) = a2 - 90` on the same branch), which flips the sign of that
derivative. Neither call site consulted `hemisphere_south_locked()`, so south of the
equator the rule was backwards on BOTH dec-axis branches. The full sign table:

| Site      | a2 >= 0 (east branch) | a2 < 0 (west branch) |
|-----------|-----------------------|----------------------|
| Northern  | negate (was correct)  | keep (was correct)   |
| Southern  | keep (was: negate)    | negate (was: keep)   |

**Why it survived.** The Wave 100i was ConformU-validated in the northern hemisphere,
where the rule is right, and the ConformU measured-rate tests that "confirmed" the sign
ran there. The southern-hemisphere hardware session (2026-09-06) exercised tracking and
`MoveAxis` -- and `move_axis()` applies no sign transform at all, so it was never
exposed to this rule.

**Fix (done).** Both call sites now negate when `(a2 >= 0) != hemisphere_south_locked()`
(XOR), which reproduces the table above. Two loopback regressions on the EQM-35 Pro
profile at latitude -37.2 assert the ASCOM contract against the driver's own pointing
model -- reported Declination RISES under `+DeclinationRate` and after a North pulse --
and both were confirmed to fail before the fix (axis moved -19.97 arcsec and -11.25
arcsec respectively, the exact mirror of the passing northern-hemisphere cases). The
existing northern-hemisphere tests are untouched and still pass.

**Still open.** This is validated against the pointing model and the loopback
simulator, not measured on the mount. The cheapest hardware confirmation is a short
autoguiding session (PHD2 calibration reports the Dec direction directly) or a
plate-solved drift run with a non-zero `DeclinationRate`. Do this before ConformU: the
suite's offset-rate tests measure the Dec direction and will fail on the old code at a
southern site.


### iOptron

Devices: Telescope (mount), Switch (iMate PowerBox), Focuser (iEAF / iAFS2/3), FilterWheel (iEFW), Camera (iCAM, via Player One SDK).

- **iCAM cameras are rebadged Player One cameras** (2026-08-26): the iCAM178M enumerates as USB `a0a0:178b` with the descriptor string "Player One iCAM178M", and the Player One SDK lists it by name with no special handling. iOptron publishes no camera SDK. The `(ioptron, camera)` router branch therefore calls `create_playerone_camera()` under `#ifdef ALPACACORE_ENABLE_PLAYERONE` (not the iOptron flag) — there is no iOptron camera class. The web UI's `ioptron-camera-index` field uses `name="ioptronCameraIndex"` (FormData collision rule) and has its own `INDEX_FIELDS` entry, so auto-numbering is per `(ioptron, camera)`; the SDK index space is still shared with Player One-branded cameras, which the UI helper text says. Any future iCAM model should be checked with `lsusb` first: VID `a0a0` means the alias just works. Validated on iCAM178M (`a0a0:178b`, mono), iCAM462C (`a0a0:462a`, color RGGB) and iCAM464C (`a0a0:464a`, color, IMX464 2712x1538), ConformU 4.5.0 clean on all three.
- **A sick USB link looks like a driver bug (bit us on the iCAM462C, 2026-08-26)**: the first ConformU run showed `CCDTemperature = -300`, `PulseGuide` failing with `POASetConfig(bool): operation failed` while `CanPulseGuide` was true, and `StartExposure` never leaving Exposing; the server log had `libusb: error [op_set_configuration] failed, error -1 errno 71` on the reopen and the camera later vanished from enumeration (`Camera index out of range`). Re-seating the camera on a direct USB 3 port produced a fully clean run with a valid temperature and working ST4 pulses. Do NOT "fix" the driver for these symptoms (guarding `has_guide_st4` / the -300 sentinel was drafted and reverted); check `journalctl` for errno 71 and `lsusb` for the device first.

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
- **HAE16 EQ (model code 0012)** shares the HAE29C GOTO final-approach quirk (11-16 arcsec east RA settle, Dec within 0.2 arcsec) on firmware 241201 but parks cleanly; the GOTO trim is gated by `goto_refine_active()` (0036 or 0012), the park finalizers by `hae29c_quirks_active()` (0036 only). When a new iOptron model shows the RA-only settle signature, add its code to `goto_refine_active()` and re-validate; do not widen the park gate without a wedged-park log.
- **HAE29C (model code 0036) firmware quirks** — all workarounds live behind `hae29c_quirks_active()` (strict equality on `0036`; HEM27/HAE43/other codes get stock behavior). Verified on hardware: (1) `:MP1#` completes physically but never reports status 6 — `:ST0#` finalizes it (driver watches for slewing-but-stationary-at-park-target); (2) zero-distance park wedges identically; (3) GOTO settles ~11–16 arcsec east in RA (final-approach sidereal gap) and re-GOTOs under ~15" deadband — the driver closes the residual with a duration-computed pulse-guide trim (RA only; Dec is accurate and Dec pulse polarity flips with pier side). If the same symptoms show up on HAE29C-EC/AA (codes 0037–0039), extend the gate — one line.
- **Stale status-cache traps**: `find_home()` must force-refresh before its already-home early-return, and `park()`/`unpark()` must invalidate cached `is_at_home` — a Park/Unpark cycle otherwise leaves a stale `is_at_home=true` that silently no-ops the next FindHome (ConformU: "AtHome reports false after FindHome").
- **ConformU 4.4 tests `Connect()` immediately after `Connected=false`** — an async disconnect must not drop a racing connect (AsyncConnectable now queues it; see `pending_connect_`). ConformU 4.3 never exercised this, so a 4.3 pass does not imply a 4.4 pass on the connect phase.
- **Debugging discipline from this session**: mount "wedges" that survive reconnects but clear on a fresh flushed port open are usually leaked-byte desync or firmware state, not dead hardware; a mount that answers probes but fails mid-motion points at the USB link (here: loose cable + VMware passthrough drop under motor load). Reproduce failing ConformU members individually with `curl` against the live server before burning full ConformU runs.

#### iEAF / iAFS2/3 (Focuser)

The iEAF electronic focuser (and the iAFS2/3 automatic focuser, which speaks the identical protocol and reports `:DeviceInfo#` model code 3 vs the iEAF's 2; per INDIGO `indigo_focuser_ioptron.c`) has its own serial protocol and its own wrapper (`ioptron_ieaf_protocol_wrapper`) — it does **not** share `ioptron_protocol_wrapper` with the mount. Reference implementation: INDI core `drivers/focuser/ieaffocus.cpp`.

- **Hardware**: built-in Prolific PL2303 USB-serial bridge. iEAF = `067b:23d3` ("USB-Serial Controller"), iAFS2/3 = `067b:23a3` ("ATEN Serial Bridge"). Auto-detect filters on the `067b` vendor ID and adapter names, never on a product ID, so both enumerate. Fixed 115200 8N1 — the config's baud field is ignored. No DTR-reset quirk (not an Arduino-class MCU), so no CH340-style HUPCL handling is needed.
- **Handshake**: `:DeviceInfo#` → `"%6d%2d%4d#"` (position snapshot, model code, firmware/build). Model code **2 = iEAF, 3 = iAFS2/3**; anything else is rejected. The reported `Name`/`Description` come from the config key `model` (`ieaf` default | `iafs2`, web UI Model selector), **not** from the handshake code. Two earlier cuts tried handshake-based naming: the first cleared the learned name on disconnect so the device list flip-flopped iAFS2/3 -> iEAF whenever a client let go, and the second added an "Auto-Detect" model option next to the "Auto-Detect" connection type, which Joey found confusing. The user knows which unit they own; let them pick. Unique ID stays `IOPTRON_IEAF_n` for both models. The iAFS2 (model code 3) passed ConformU 4.5.0 on 2026-08-23 with the same driver; its report was captured while the display name was still "iAFS2", before it was widened to "iAFS2/3" for the 2" and 3" variants (text-only change). This is also the auto-detect probe — important because the iOptron mount USB port enumerates as the same Prolific chip class, so the model-code check is what tells a mount and an iEAF apart on a box with both plugged in.
- **Status**: `:FI#` → `"%7d%1d%5d%1d#"` = position, moving flag, temperature in **Kelvin × 100** (`t / 100.0 - 273.15` for °C), direction flag (0 = reversed).
- **Command acks**: `:FM%7u#` move-absolute (space-padded 7-wide, matching INDI), `:FQ#` abort, `:FZ#` set-current-position-as-zero, `:FR#` toggle direction (not used by the driver — ASCOM Focuser has no reverse property). Each answers with a **single `1` byte and no `#`** (INDI reads exactly one byte after them). The wrapper consumes that ack with a 300 ms timeout: left in the buffer it races the `tcflush` at the start of the next `:FI#` and, when it lands after the flush, prefixes the reply (`1+0132110299271#`) and breaks the fixed-width parse. Seen on hardware as an intermittent "Failed to parse iEAF status" on ConformU's Move. Completion is observed by polling `:FI#`.
- **Status cache**: one `:FI#` round trip is ~10 ms on the wire; the driver serves IsMoving/Position/Temperature from a 100 ms cache (dropped by move/halt/disconnect) so `DeviceState` is one poll, not three. The original 50 ms pre-read sleep in `send_command_locked` was most of the cost — `read_response()` already waits for `#`, so it is 5 ms now.
- **Timing marks that are not yours**: when ConformU runs *on the Pi* alongside the server, a cold ConformU process shows 0.1-0.25 s on `DeviceState`/`MaxStep` (a constant!) while the server log shows the request arriving 200+ ms after the previous reply was sent. That is ConformU-side .NET warm-up; re-run in the same ConformU session and it clears. Prove it with the TRACE log request timestamps before touching the driver.
- **Enum-typed members carry a ~100 ms first-use JIT cost in ConformU on a Pi 4-class core** (HAE16 EQ session, 2026-08-25): across five runs the only FAST marks were `AlignmentMode`, `EquatorialSystem` and `SideOfPier` at 0.095-0.15 s, and two of those are hard-coded constants with no lock or serial. Every bool/double/string/int member was 5-15 ms; `DestinationSideOfPier`, the *second* use of the `PierSide` enum, was 3 ms. Each is the first response of a distinct enum type, i.e. .NET tiered JIT of the generic deserialize path per `T`. Eliminated on our side: `curl` timing of the exact PUT-then-GET sequence is 1-2 ms, `performance` governor made no difference, and a dual-stack listener (removes the refused IPv6 SYN .NET sends first) made no difference. Whether it clears on a warm re-run depends on the member type -- it did not for these enum-typed mount members (unlike the cold-process marks above), while a camera pass can come up clean simply because the first-use cost was already paid earlier in the same process. Either way a clean 4.5.0 timing record is not evidence of a real pass; only a 4.5.1 re-run settles one and the same code passed clean on the faster astro.lan rig. Do not chase it in the driver. Reported upstream as ASCOMInitiative/ConformU#31 and fixed in ConformU 4.5.1 (same HAE16 build: 0 timing marks). Use 4.5.1 or later on Pi-class hosts. The old workaround for 4.5.0 -- run the pass twice in one ConformU web-UI session and keep the warm log -- is **withdrawn on arm64**: the 4.5.0 linux-arm64 asset was published without `PublishReadyToRun` (see the Target Architecture note), so a warm log there hides a build-wide defect rather than a one-off cold start, and `/conformu` step 2g now replaces an installed 4.5.0 before any run. Replace ConformU; do not warm-log around it.
- **Diagnosing "server slow?" from a remote browser shell**: `curl -s -o /dev/null -w "%{time_total}"` loops for the exact member sequence, then `tcpdump -i lo -w /tmp/x.pcap port 6800` (capture to file; `-A` to a browser shell is unusable) read back with `-r ... -A | grep -E "Flags \[S\]|GET /api|^HTTP/1.1 200"`. The tcpdump is what exposed the IPv6-first connects.
- **Range**: 0..99999 steps (INDI's FocusAbsPos max; the 7-digit move field could carry more but the hardware range is 5 digits). Step size in microns is not exposed — connected, `StepSize` throws `PropertyNotImplemented`; disconnected it is `NotConnected` (#309). No temperature compensation in hardware.
- ConformU 4.5.0 validated 2026-08-23 on iEAF hardware (model code 2, firmware 100), Raspberry Pi arm64: 0 errors, 0 issues, all timing targets met.

#### iEFW (FilterWheel)

The iEFW-15 (5 slots) and iEFW-18 (8 slots) share the iEAF handshake family but have their own wrapper (`ioptron_iefw_protocol_wrapper`). Reference: INDI core `drivers/filter_wheel/ioptron_wheel.cpp` (INDIGO has no iOptron wheel driver).

- **Hardware**: Prolific PL2303 USB-serial bridge like the focusers, fixed 115200 8N1.
- **Handshake**: `:DeviceInfo#` → 12 digits, parsed `"%6d%2d%4d"`; digits 7-8 are the model. **99 = iEFW-15 (5 slots), 98 = iEFW-18 (8 slots)** per INDI, confirmed on Joey's iEFW-15 (code 99, firmware 100). The slot count comes from the handshake; the reported `Name` comes from the config key `model` (`iefw15` default | `iefw18`, web UI Model selector, the user-choice pattern Joey asked for on the focuser), and a mismatch is logged as a WARN while the wheel's slot count wins. Lesson from this session: confirm which unit is physically attached before concluding a vendor table is wrong (an hour went into "the code is untrustworthy" when the 15 was simply the wheel on the bench). The focusers answer the same command with 2/3 and the mount with its own codes; the model check is what keeps auto-detect from grabbing them.
- **Commands**: `:WP#` → `nn#` current slot (0-based) or `-1#` while moving; `:WMnn#` move (2-digit, 0-based) answers a bare `1` (no `#`), consumed by the wrapper for the same leaked-ack reason as the iEAF; `:FW1#` → 12-char firmware string (web UI only); `:WFnn#` / `:WOnnsnnnnn#` read/write per-slot focus offsets **stored in the wheel**; `:WF` seeds `FocusOffsets` at connect when config has none (Player One convention), `:WO` is never sent.
- **IR sensor light (hardware, not fixable in software)**: the slot-index IR LED is on whenever the wheel is powered, regardless of commands or `:WP#` polling (checked on the iEFW-15 with a phone camera while idle and while polled). No protocol command touches it; iOptron says it needs a new board. Don't spend time on polling-rate mitigations; suggest a physical baffle.
- **iEFW-18 mechanics (from the hardware owner, reported to iOptron but not on their site)**: it is **two stacked 5-slot wheels**, each with one unthreaded open slot so the other wheel's filter can be in the light path, i.e. **8 usable positions, not the advertised 9**. The firmware reports 8 (model code 98). Do not "correct" the slot count to 9. Moves that need both wheels to turn take longer than single-wheel moves; ConformU's slot walk covers both cases.
- **Position latch**: the driver reports `-1` from the moment a move is commanded until `:WP#` returns the target, so a poll that lands before the motor starts does not show the old slot as arrived.
- ConformU 4.5.0 validated 2026-08-23 on an iEFW-15 (model 99, firmware 100), Raspberry Pi arm64: 0 errors, 0 issues, timing clean on a warm ConformU session. Server-side `:WP#` is ~18 ms; the first-DeviceState 0.116 s seen on cold ConformU processes was client-side (request logged at .999, reply at .017, ConformU stamped the call as starting at .109). iEFW-18 (model 98, 8 slots) validated the same day, also clean; its move times show the stacked-wheel mechanics (1.4-1.6 s within a wheel, 3.0-3.2 s when a move crosses 3->4 or 7->0 and both wheels turn).

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
- **Every hidapi *library* call goes through `hid_global_mutex()`** (file-local in `astroasis_protocol_wrapper.cpp`): `hid_init`, `hid_enumerate`/`hid_free_enumeration`, `hid_open_path`, `hid_close`. hidapi guarantees only that *distinct `hid_device` handles* may be used from different threads; its init context and bus scan are process-global, and the wrapper's `Impl::mutex_` is per instance, so it cannot order `enumerate_astroasis_focusers()` on an HTTP thread (that is `create_astroasis_focuser_by_index()`, which enumerates eagerly) against another instance's `connect()`. Per-handle `hid_write`/`hid_read_timeout` deliberately stay **outside** that lock — the connect handshake blocks in `hid_read_timeout` for up to `kHandshakeTimeoutMs`, and holding a global lock across it would serialize every other focuser's I/O for no safety gain. Lock order: driver `mutex_` → `Impl::mutex_` → `hid_global_mutex()`; never take a wrapper lock while holding the global one. Astroasis is the only vendor linking hidapi — if a second one ever does, promote this mutex to a shared header, since two separate mutexes serialize nothing.
- The onboard temperature sensor is an NTC thermistor read through a 12-bit ADC and converted via a Steinhart-Hart-style curve ported byte-for-byte from the vendor DLL's data section (see `raw_adc_to_centidegrees()` in `astroasis_protocol_wrapper.cpp`) — the formula constants have not been cross-checked against a real temperature reading, only against the DLL's own math.
- `StepSize` and the temp-comp SETTER are not exposed by the device/protocol — both throw `PropertyNotImplemented`, don't try to synthesize a value. The two temp-comp getters answer `false` when connected. All four are `NotConnected` while disconnected (#309).
- ConformU 4.4.0 validated on Debian 13 arm64: **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/Astroasis/Oasis Focuser/`.

### Gemini

Devices: Focuser (Automatic Astro Focuser Pro), CoverCalibrator (Astro Flat Panel Cover Lite, Astro Automatic FlatPanel v2, Motorized Flat Panel V3), Switch (Power & Data Hubs Advanced 3).

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

#### Astro Automatic FlatPanel v2 (CoverCalibrator, motorized cover)

ConformU 4.4.0 validated on Debian 13 arm64 (firmware 408): **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/Gemini/Astro Automatic FlatPanel v2/`.

A second, distinct hardware model from the Astro Flat Panel Cover Lite above — same vendor, same `>X#`/`>Xnnn#` wire syntax family and shared port auto-detection (`enumerate_gemini_flatpanel_ports()`/`is_flatpanel_handshake_reply()` are model-agnostic), but this one has a real motorized cover. Selected via a `flatPanelModel` config field (`"lite"` default / `"v2"`) on the same `gemini`+`covercalibrator` router slot as the Lite model — `GeminiFlatPanelProtocolWrapper` takes a `FlatPanelModel` in `FlatPanelConnectionConfig`, and there's a second driver class (`GeminiFlatPanelV2Driver`) alongside `GeminiFlatPanelDriver` in the same `gemini_flatpanel_driver.{h,cpp}` files (not split into separate vendor files — this is a variant of the same device family, matching how the Lite model already shares files with the focuser's vendor CMake target).

Protocol: unlike the Lite (reverse-engineered from vendor app traffic), this model has no protocol docs, spec, OR vendor app traffic capture available. Everything about its cover/status commands comes from INDI's open-source `GeminiFlatpanelRev2Adapter` (`indilib/indi`, `drivers/auxiliary/gemini_flatpanel_adapters.{h,cpp}`), which itself documents this as one of four INDI-supported variants (Rev1/Rev2/Lite/Pro) sharing one INDI driver. Light/brightness commands (`>L#`/`>D#`/`>B<value>#`/`>J#`) are confirmed shared with the Lite model (same INDI source), so those are lower-risk than the cover/status parsing below.

- **Handshake**: `>H#` → `*HGeminiFlatPanel#` (no `"Lite"` suffix — distinguishes it from the Lite reply at the protocol level, though the driver doesn't currently enforce that distinction; `is_flatpanel_handshake_reply()` stays generic to any `*H`-prefixed reply for both models, same as it always has).
- **Firmware minimum**: INDI's adapter rejects firmware < 402 outright. This driver only logs a warning at that threshold rather than failing connect — not confirmed whether older field units would actually misbehave.
- **`>S#` status reply has a DIFFERENT layout from the Lite's**: `*S<2-digit id><motor><light><cover>#` (7+ chars) vs. the Lite's `*S<light><f2><f3>#` (6 chars) — see `parse_light_flag()` vs. `parse_rev2_status()` in `gemini_flatpanel_protocol_wrapper.cpp`. The 2-digit device ID must be `19` or `99` (INDI's adapter gate) or the reply is rejected as unparseable. `motor`/`light`/`cover` are each a single digit; `cover` values match INDI's `GeminiCoverStatus` enum exactly: `0`=Moving, `1`=Closed, `2`=Open, `3`=TimedOut (mapped to ASCOM `CoverState::Error`).
- **Cover commands block the wire until physically done**: `>O#` (open) waits for the exact reply `*OOpened#`, `>C#` (close) waits for `*CClosed#`, both with a 30s timeout (`kCoverMoveTimeoutS`) — the reply IS the completion signal, there's no separate "done" query. Since ASCOM's `OpenCover`/`CloseCover` are async initiators (must return in ~1s per the STANDARD timing target, not block for the full travel), the driver runs these on a background thread (`start_cover_task()` in `GeminiFlatPanelV2Driver`) rather than calling them synchronously from the HTTP handler — same "joinable member thread, joined before starting a new one, reaped in the destructor" idiom as the Celestron/iOptron/SynScan telescope drivers' async slew tasks (see their `slew_task_thread_`/`slew_dispatch_thread_` members) — there was no existing generic reusable helper for this pattern to build on. A second open/close call while one is already in flight throws `InvalidOperation` rather than blocking or queuing.
- **`HaltCover` has no hardware equivalent on Rev2** — per INDI's `GeminiFlatpanel::AbortCap()`, only the separate "Pro" hardware revision returns success; Rev1/Rev2/Lite all fail it. Because this model's `CoverState` is a real state (not `NotPresent` like the Lite), the "unconditional `MethodNotImplemented`" pattern used for the Lite's cover methods does NOT apply here — per the WandererCover precedent (see that section below), ConformU requires `HaltCover` to function on any cover-capable device. Fix: `HaltCover` sends no wire command; it just sets a `cover_halted_` flag that makes `CoverState`/`CoverMoving` stop reporting `Moving` (`CoverState` returns `Unknown` while halted) immediately, while the in-flight open/close command's blocking wire call keeps running in the background until the motor reaches its end stop or the 30s timeout fires on its own.
- **`CalibratorOn`/`CalibratorOff` must run on a background thread too, not just Cover** — `GeminiFlatPanelProtocolWrapper::Impl` serializes ALL wire commands (light/brightness AND cover) behind one `mutex_`, because there is only one physical serial link and interleaving bytes from two commands would corrupt both. `open_cover()`/`close_cover()` hold that mutex for up to `kCoverMoveTimeoutS` (30s) while blocked reading the motor's completion reply. The first implementation called `protocol_.light_on()`/`set_brightness()` directly from `calibrator_on()` on the HTTP handler thread; ConformU caught this by issuing `HaltCover` (which per the note above does NOT actually stop the wire-level open/close call) on a cover that was still physically mid-travel, then immediately calling `CalibratorOn` — which blocked for 6+ seconds waiting for the cover mutex, blowing the 1.0s STANDARD target. **Fix**: `calibrator_on()`/`calibrator_off()` now dispatch to a background thread (`start_calibrator_task()`, mirroring `start_cover_task()`) so the ASCOM initiator returns immediately regardless of what protocol_'s mutex is doing, with `CalibratorChanging`/`get_calibrator_state()` reporting `NotReady` while the background command is in flight (checked via `calibrator_pending_count_`, an atomic **counter** — deliberately not the single `cover_in_flight_` bool the cover path uses. Calibrator commands chain behind each other, so an earlier thread finishing and clearing a shared bool would report "not changing" while a later-queued thread is still running its own wire command; each thread increments the count for its own command and decrements it on exit, and `NotReady` holds until the LAST queued command finishes. Use a counter for any initiator that can be re-entered before the previous one drains). This is also just the textbook-correct ASCOM `CalibratorOn` implementation per ConformU's own guidance ("return quickly... set CalibratorChanging to true...") — worth defaulting to for any calibrator that shares a wire/bus with another slow operation, not just as a fix for this specific contention. Lesson generalizes beyond Gemini: **any driver where two ASCOM-exposed operations share one underlying serial/bus mutex needs BOTH operations to be asynchronous**, not just the one that happens to be slow on its own.
- **`CoverState`/`CoverMoving` are never cached** (unlike `Brightness`/`CalibratorState`, which — same as the Lite — are driver-tracked so ConformU's immediate post-`CalibratorOn` read gets the just-set value, not a stale one). Cover state genuinely changes on its own mid-flight in a way the driver doesn't control tick-by-tick, so `get_cover_state()` queries `>S#` live whenever no cover task is in flight and isn't halted.
- Default baud rate 9600 (8N1), same connection type options (`connectionType`: `auto`/`serial`) as the Lite; auto-detect shares `enumerate_gemini_flatpanel_ports()`/`probe_port()` with the Lite model verbatim, including the DTR/HUPCL and Espressif-vs-CH340 handling documented above — nothing v2-specific was needed there since no Rev2 unit was available to confirm whether its by-id pattern differs.
- Web UI: `gemini-flatpanel-model` select toggles between `gemini-flatpanel-lite-fields` and `gemini-flatpanel-v2-fields` sub-blocks within the shared CoverCalibrator section; `flatPanelModel` (`"lite"`/`"v2"`) is the persisted config field. The v2 index field (`gemini-flatpanel-v2-index`) shares its `INDEX_FIELDS` `configKey` (`panelIndex`) with the Lite's, since both models compete for the same auto-detect index namespace.

#### Motorized Flat Panel V3 (CoverCalibrator, motorized cover, INDI "Pro" firmware)

ConformU 4.5.0 validated on Debian 13 arm64 (Raspberry Pi, firmware 107): **0 errors, 0 issues, 0 timing issues**. Results in `AlpacaCore/conformu/Gemini/Motorized Flat Panel V3/`.

Third model on the same `gemini`+`covercalibrator` slot (`flatPanelModel: "pro"`). Sold as "Gemini Motorized Flat Panel V3"; on the wire it is the variant INDI's `gemini_flatpanel_adapters.cpp` calls **Pro** (`GeminiFlatpanelProAdapter`). It shares `GeminiFlatPanelV2Driver` with the v2 model (identity strings switch on `config_.model`; new `create_gemini_flatpanel_pro[_by_index]()` factories) - only the protocol wrapper parses differently. Everything below was confirmed on real hardware, not just INDI.

- **Identify by the handshake, not the store name**: `>H#` -> `*HGeminiFlatPanelPro#` (Lite: `...Lite#`, Rev2: `*HGeminiFlatPanel#`). `expected_flatpanel_handshake_reply(model)` returns each; `connect()` warns (does not fail) when the reply doesn't match the configured `flatPanelModel`, because each model parses `>S#` differently and a mismatch shows up as garbage status later. Auto-detect stays model-agnostic on the `*H` prefix.
- **`>S#` layout is positional with letter tags**: `*S0M0L2C0D76C405O#` = motor at [2], light at [4], cover at [6] (`0`=Moving `1`=Closed `2`=Open `3`=TimedOut, same enum as Rev2), then extra fields INDI ignores (`0D` presumed dew heater, `76C`/`405O` presumed close/open position calibration). No 2-digit device-ID gate like Rev2's `19`/`99`. See `parse_pro_status()`.
- **Open/close acks are NOT `*OOpened#`/`*CClosed#`**: firmware 107 answers `>O#` with `*O405#` and `>C#` with `*C70#` (the position it reached; the open value varied 403-415 across runs), arriving only when travel finishes (~10 s). INDI's Pro adapter accepts any `*O`/`*C` prefix; so does `is_cover_reply_ok()` for the Pro model only - Rev2 keeps the exact-string check it was validated with.
- **No wire-level abort on Pro either**: INDI's `AbortCap()` returns OK for Pro without sending anything, so `HaltCover` keeps the v2 "stop reporting Moving" behaviour. Firmware 107 has no minimum-version gate (INDI's 402 check is Rev2-only).
- **Live `CoverState` must be under 0.1 s and the 18-char reply made it 0.13 s**: raw `>S#` is 27 ms wire-to-wire on the Pi, but `send_command_locked()`'s 50 ms `kCommandDelayMs` settle before the first read pushed the round trip over ConformU's FAST target. The Pro path now skips that settle (Lite/Rev2 keep it - no unit on hand to re-validate them without it); live `CoverState` is ~28 ms. `get_cover_state()` additionally serves the idle read from a 1 s TTL cache (`kCoverStatusCacheMs`) that is seeded from the connect-time status read and invalidated when a cover task completes, so ConformU's post-Connect sweep (where even constant properties cost ~0.08 s client-side, see the OnStep notes) is a cache hit and the first post-move read always goes to the wire.
- **ConformU test-order trap for any cover device: start the run with the cover CLOSED.** `CoverCalibratorTester` orders the cover tests on the initial `CoverState`: Closed -> Open/Close/Halt, but Open -> Close/**Halt**/Open. Its `canAsynchronousOpen` flag is only set inside `TestOpenCover()`, so on the Open path `TestHaltCover()` reads the default `false` and logs "The cover operates synchronously but did not throw a MethodNotImplementedException" against a perfectly asynchronous driver (run 1 of this validation). Not a driver bug; the v2 log only passed because that cover happened to start closed. Close the cover before every CoverCalibrator ConformU run.
- **Light commands cost ~125 ms EACH in the Pro firmware** (`>L#`, `>D#`, `>B<n>#` all measured 123-129 ms raw on the Pi, vs 27 ms for `>S#`), and `>B<n>#` alone does NOT light the panel (the `>S#` light flag stays 0 until `>L#`), so a toggle is two commands = ~250 ms hardware floor. `CalibratorOn`/`CalibratorOff` in the shared v2/Pro driver now take a **synchronous fast path** when no cover move or earlier calibrator command is in flight (checked under `cover_task_mutex_` so a move can't start mid-command): the call returns with `CalibratorState` already `Ready`/`Off`. The background-thread path (the ConformU-contention fix documented for v2 above) is kept only for the cover-in-flight case. Reason: with the always-async path, NINA's toggle looked multi-second because NINA polled `CalibratorState` slowly while the light had actually flipped in 250 ms.
- Extra Pro-only commands exist (`>M±nn#` manual jog, `>E#`/`>F#` set open/close position) and are deliberately not exposed - out of ASCOM CoverCalibrator scope.

#### Power & Data Hubs Advanced 3 (Switch, power box)

`gemini` + `switch`, `switchType: "pdh-adv3"` (discriminator from day one; the PowerBox Mini 2 is the obvious second backend). Files: `gemini_pdh_protocol_wrapper.{h,cpp}` + `gemini_pdh_switch_driver.{h,cpp}`, template = the WandererBox Pro V3 switch driver. Hardware: 4x switched 12 V (DC2-DC5) + DC1 always-on, 6x switchable USB (A/B = USB 3.2 Gen1, C-F = USB 2.0), 2x PWM dew heaters (DEW6/DEW7) with Auto (PID) / Manual / Switch modes, AHT20 ambient temp+humidity on the "Temp" port, DS18B20 lens probe on the "Dew Temp" port, input V / output A / W telemetry. Both sensors are meant to be used together: Auto mode is the PID loop that keeps lens temperature above the AHT20 dew point.

Protocol: **no docs, no INDI/INDIGO driver, no vendor app capture** -- everything came from decompiling the vendor's Windows ASCOM driver (`ASCOM.GeminiPowerBoxPlusAdv3.Switch.dll` v2.6.0206, .NET, Inno Setup installer from geminiastro.cc/downloads; rootless `innoextract` + `ilspycmd` recipe in `/driver-build` Question 5a). Summary in `AlpacaCore/external/Gemini/PowerDataHubAdv3-protocol.md`; the decompiled source is NOT committed. Same `>X#`/`*X...#` family as the flat panels but a different command set and **19200 baud** (the panels are 9600), CH340/CH341 bridge (vendor ReadMe requires the CH341 driver), `>H#` -> `*HGeminiPowerBoxPlusAdv3#` compared verbatim, `>V#` -> 3-digit firmware (308 = 3.0.8, vendor refuses < 308 and so do we), `>G#` -> one positional status frame the vendor splits on the letters `DUATMBCSHVP` (17 fields: 4 DC digits, 6 USB digits, AHT/DS18 flags, DEW6/7 enabled+mode, DEW6/7 manual %, lens/ambient/humidity/dew point, V/A/W). Set commands (`>O<n>#`/`>C<n>#` outputs 1-11, `>X`/`>Y` manual PWM, `>Z1x`/`>Z2x` enable, `>M1x`/`>M2x` mode) are fire-and-forget; the vendor never reads their reply.

- **Commands go out with a trailing `\n`** (`SerialPort.WriteLine`), unlike the flat panel wrapper's bare `>X#`. Mirror the vendor: the firmware is only proven against that framing.
- **Reader thread owns every read, handshake included.** The vendor's sensor window reads whatever is in the buffer every 3 s without sending anything and parses it as a `>G` frame, which strongly suggests the firmware streams status on its own. The wrapper routes any `*G` frame to the cache and only letter-matched frames to the one pending request (`>H#`/`>V#`/`>G#`), and re-polls `>G#` when the cache is older than 2 s -- so Switch reads never touch the wire (FAST target) and an unsolicited frame can never be mistaken for a handshake reply. TODO(hardware): confirm streaming vs. reply-only, and the actual tag letters/order of the frame.
- **Auto-detect must match the exact identity string.** The focuser, all three flat panels and the hub enumerate as the same CH340 by-id names; accepting any `*H` reply would grab a flat panel. `is_pdh_handshake_reply()` is an exact compare; a `*HGeminiPowerBox...` reply that is not `...PlusAdv3` is logged as an unsupported model (the previous-generation PowerBox Plus V3 has a different frame). The probe reads frames until the deadline instead of taking the first `#`-terminated reply, for the streaming reason above.
- **DEW output switches follow the vendor's mode-dependent range**: 0-100 (`>X`/`>Y` PWM) in Manual, 0-1 (`>Z` enable) in Auto/Switch. `MaxSwitchValue` therefore reads the channel's *effective* mode (commanded this session, else firmware-reported). The mode switches (0 Auto / 1 Manual / 2 Switch) are **writable at runtime and never persisted** (project thermal policy -- the vendor persists them in its setup dialog; we deliberately don't). Auto without both sensors attached is allowed with a warning; the firmware itself falls back to Manual (vendor logic).
- Commanded-value semantics for every writable switch (WandererBox/ETA lesson): set commands are blind and the frame lags by a poll period, ConformU reads back immediately. A post-write `>G#` request refreshes the cache sooner than the periodic poll.
- Read-only telemetry as switches (V/A/W, ambient temp/humidity/dew point, lens temp, two "sensor attached" flags): the vendor only shows these in a pop-up window; exposing them is what makes them usable in NINA. Absent-sensor convention -127 degC / 0 % (WandererBox).
- **ConformU 4.5.0 validated on real hardware (firmware 3.0.9, 2026-09-08): 0 errors, 0 issues, 0 timing issues** on the Raspberry Pi rig (`openastro.lan`, ConformU on the Pi against localhost; DeviceState 43 ms, everything else under 20 ms) -- that log is the one in `AlpacaCore/conformu/Gemini/Power & Data Hubs Advanced 3/`. An earlier run on the dev VM against localhost (hub on VMware USB passthrough) was equally clean (slowest 20 ms), so this driver is fine to smoke-test on the VM before a Pi deploy.
- **Link health (issue #237, 2026-09-09)**: reads are cache-only by design, so the reader thread latches a link fault after 3 consecutive unanswered `>G#` polls (~6 s; a failed poll write counts too), clears `PdhState::valid`, and the driver throws `DriverException` on every value read/write (commanded values included) until a frame arrives again. Found on the VM rig: USB passthrough re-enumerated, the hub served byte-identical telemetry for 30 min while `SetSwitchValue` failed with EIO. See "Cache-backed reads must track link health" above.
- **Hardware findings (confirmed 2026-09-08)**: `>H#` -> `*HGeminiPowerBoxPlusAdv3#` verbatim; `>V#` -> `*V309#`. The `>G#` frame uses **suffix** tags with fixed-width, space-padded numbers: `*G1111D111111U1A1T1A1M1B1M100C100C 24.06S 23.39T 42.04H  9.76D12.6V 0.11C  1.38P#` -- the split-on-tag-letters parser handles it because `strtod`/`strtol` skip leading spaces (a stricter "all digits" parse of the numeric fields would have broken). **The firmware streams `*G` frames at ~3 s only after the first `>G#`** (nothing unsolicited for the first ~5 s after open, then continuous once queried) -- so the reader-thread design was necessary: a request/response wrapper would have read a streamed frame as the `>V#` reply. Set commands produce no ack. Both dew channels came up Manual at 100 % on power-up.
- **All writable-switch writes are serialized under `write_mutex_`** (validate + send + record as one step; readers never take it). PR #236's bot review spotted that a DEW output's mode-dependent max was read in one lock and the commanded value recorded in another, so a concurrent mode write could let a value be validated against a stale max. A concurrent mode/value writer stress case covers it.
- Hardware-free Catch2 coverage in `test_gemini_pdh_switch.cpp`: driver-contract, frame-parsing and handshake cases (incl. the real captured frame above) plus cases over a pty-backed fake hub (`tests/fake_gemini_pdh.h`: replies to `>H#`/`>V#`/`>G#`, never acks set commands, optionally streams `*G` frames) covering connect + first frame, every write path incl. the mode-dependent DEW range, streamed frames routed past the handshake, the 2 s stale re-poll, the firmware gate, and concurrent DEW mode/value writers. Plus routing/config round-trip.

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
- **Link health for the whole streaming family (issue #237, 2026-09-09)**: the cover, SFW and box wrappers all latch a link fault after 10 s without a frame (`util::StreamLinkHealth`, the last `read()` errno named in the reason) and clear `valid`; the box throws `DriverException` on value reads/writes, the cover reads `CoverState`/`CalibratorState::Unknown` and refuses commands, the wheel throws on `Position`/moves. Same rule as the Gemini PDH; see "Cache-backed reads must track link health".

### WeeWX

Devices: ObservingConditions.

No external SDK — reads weather data from a local WeeWX weather station instance.

### OnStep

Devices: Telescope.

No vendor SDK — OnStep is open-source LX200-protocol firmware (Arduino Mega/Due, Teensy,
ESP32, STM32) for DIY/retrofit mounts. No manufacturer protocol PDF exists; the reference for
this driver is INDI's `lx200_OnStep` driver (`indilib/indi`, `drivers/telescope/lx200_OnStep.cpp`
+ the shared `lx200telescope.cpp`/`lx200driver.cpp` base it inherits from) — there was no INDI
driver naming collision to resolve, "OnStep"/"On-Step" appears verbatim in both.

Connection types: **Serial (USB) only** in this project — no Wi-Fi/network config is exposed to
users, unlike iOptron/Celestron. Default 9600 baud, 8N1, standard LX200 `#`-terminated ASCII
commands. `onstep_protocol_wrapper`'s `ConnectionType::Network` branch still exists internally
(mirrors the SynScan wrapper) purely as a test seam so the mandatory `[stress]` concurrency test
can drive the driver to CONNECTED against a loopback `FakeMountServer` without real hardware —
it is never reachable through `router.cpp` or the web UI.

- **Auto-detection**: probes `/dev/serial/by-id/` (Prolific/FTDI/CP210x/Silicon Labs/CH340/
  CH341 substrings, plus `Arduino`/`Teensy`) falling back to `/dev/ttyUSB0-9` **and**
  `/dev/ttyACM0-9` — OnStep boards commonly enumerate as ttyACM (native USB-CDC) rather than
  ttyUSB, unlike the RS-232-based mounts (iOptron/Celestron) this project supported first.
- **Identity probe**: `:GVP#` (get product name) must return `"OnStep"` or `"On-Step"`;
  cross-checked with `:GVN#`'s leading version digit to distinguish legacy OnStep from OnStepX
  per the INDI driver's own detection logic.
- **DTR/HUPCL Arduino-reset quirk**: like the Gemini focuser wrapper, CH340/CH341/native-USB-CDC
  adapters reset the MCU on a DTR edge at port open. The probe path (`probe_onstep_port()`)
  clears `HUPCL` before *and* after probing, and the production `connect_serial()` path also
  clears `HUPCL`, so the driver's real connect (which follows a successful probe) never
  re-triggers the reset.
- **Pulse guiding is hardware-timed**: OnStep supports native duration-bearing guide commands
  (`:Mgn####`/`:Mgs####`/`:Mge####`/`:Mgw####`, 4-digit zero-padded milliseconds) — fire-and-forget,
  the mount stops the pulse itself. `IsPulseGuiding` is tracked from the known requested
  duration, not polled from the mount. Unlike SynScan (no native pulse guide), this driver does
  **not** need a background stop-thread.
- **SideOfPier must be computed from hour angle, not read from `:GU#`'s `E`/`W` flag** — the same
  lesson iOptron already learned from its `:GEP#` raw pier value (see the iOptron notes above):
  a mount's raw physical-pier-side report does not match the ASCOM convention once tracking past
  the meridian. `:GU#` does encode pier side directly (confirmed on real hardware, firmware
  "On-Step" v10.23a, characters `E`/`W`), and an earlier version of this driver preferred that
  flag over the hour-angle computation, falling back to hour angle only when neither character
  was present. This parsed correctly (verified against the exact character table below) but
  failed ConformU's SideofPier check on real hardware: `Reported SideofPier at HA -9, +9: EE`
  (constant, reflecting the physical side, which only changes on an actual mechanical meridian
  flip) instead of the required `WE` (flips with HA sign — ASCOM's pointing-state contract).
  `get_side_of_pier()` now always computes from hour angle (`LST − synced RA`); `:GU#`'s `E`/`W`
  is parsed and available in `MountStatus` but intentionally unused for this property. Do not
  reintroduce a firmware-flag-preferred branch here without re-validating against ConformU's
  SideofPier/DestinationSideofPier checks. Also avoid forcing a live position refresh inside
  `get_side_of_pier()` (a `:GRa#`/`:GDe#` round trip, ~0.13s each on this serial link, can push
  the read over ConformU's 0.1s FAST target) — hour-angle *sign* tolerates a couple of seconds of
  RA staleness, so reuse whatever is already cached and only force a fetch if never yet populated.
- **`:GU#` status-character table**, confirmed against real hardware traces (`nNpHrsEo180` etc.
  observed live): `n`/`N` = **NOT** tracking / **NOT** slewing respectively (`== npos` check, i.e.
  the character's *absence* means the state is active — this is the opposite polarity of what
  the INDI driver's comments implied, and was the single biggest early bug in this driver).
  `H` = at home, `P`/`p` = parked/not parked, `E`/`W` = pier side east/west. Any other character
  is ignored (forward-compatible with firmware revisions); only a total non-response is a fault.
- **RA/Dec position reads use the high-precision variants**: `:GRa#`/`:GDe#` (fractional
  seconds/arcseconds: `HH:MM:SS.SSSS#` / `sDD*MM:SS.SSSS#`), not `:GR#`/`:GD#` (whole-second/
  whole-arcsecond resolution). The whole-second RA resolution (~15″ at the equator) is far too
  coarse for ConformU's 0.07″ PulseGuide tolerance on short (2-5s) guide pulses — confirmed via
  ConformU issue count dropping from 8 to 0-2 after switching. `parse_sexagesimal()` handles the
  fractional last field transparently (`std::stod` on a run of digits+`.`), no parser change
  needed.
- **MoveAxis uses a true arbitrary continuous rate, not a discrete preset**: `:RA[n.n]#`
  (East/West, RA axis) / `:RE[n.n]#` (North/South, Dec axis) set a real custom rate in
  degrees/second — confirmed against `Command.ino`/`Guide.ino`: setting one flags
  `currentGuideRate=-1`, and `:Mn#/:Ms#/:Me#/:Mw#` then resolve through `enableGuideRate(-1)`,
  which uses this custom rate instead of a discrete preset. **This was the second-biggest bug**:
  an earlier version of this driver snapped the requested rate to the nearest of OnStep's 4
  discrete named presets (`:RG#`/`:RC#`/`:RM#`/`:R9#` — Guide/Center/Find/Max, the classic
  hand-paddle rate set) and reported `AxisRates` as 4 single-point ranges to match. This looked
  ASCOM-conventional but broke real-world use: N.I.N.A.'s manual slew pad (native Alpaca
  connection) disabled both the direction buttons and the rate stepper entirely when given
  single-point (Minimum==Maximum) ranges — confirmed by a user testing against live hardware
  ("rien ne bouge" / nothing moves, rate stepper non-interactive). `AxisRates` must report ONE
  continuous range `[0, kMaxMoveAxisRateDegPerSec]`; `move_axis_start()` sends the exact
  requested rate via `:RA#`/`:RE#` rather than snapping to a tier. Do not reintroduce the
  discrete-preset approach even though it matches some other drivers' hand-paddle-only UX —
  OnStep's firmware supports true arbitrary rates and ASCOM clients expect to use them.
- **Continuous-motion rate is per-move, not fixed at connect**: `select_max_slew_rate()` (`:RS#`,
  half-max) is still called once at connect as a harmless safe default, but the real rate for
  every `MoveAxis` call is set immediately before each `:Mn#/:Ms#/:Me#/:Mw#` via the mechanism
  above.
- **Tracking on/off**: confirmed on real hardware to be `:ST60.164275#` (start — sets tracking
  rate to exact sidereal, `60.164275 = 60.0 × 1.00273790935`) / `:ST0#` (stop — any rate below
  0.1 Hz reads as `TrackingNone`). `:To#`/`:Tn#` (this driver's original guess, inferred from the
  INDI driver's tracking-*compensation* switches) do **not** toggle tracking — they instead
  control `rateCompensation` (refraction/full-model RA-rate compensation, `:Tr#`/`:Tn#`/`:To#`),
  a completely different feature (confirmed via firmware source `Command.ino` and an A/B test on
  real hardware). Do not reuse `:Tr#`/`:Tn#`/`:To#` for tracking on/off.
- **Longitude is West-positive on the wire**, despite ASCOM's East-positive convention and
  despite the lenient parser accepting either sign silently. Confirmed empirically on real
  hardware via a GMST/LST cross-check (mount's own `:GS#` vs. independently-computed sidereal
  time at matching instants): longitude=0 gave a near-exact match; the correct (site) longitude
  with the ASCOM sign gave a large, systematic LST error; negating it gave an exact match to the
  second. `format_longitude()` negates on write, `get_site_info()` negates on read.
- **Local time is sent as UTC directly, with offset=0.0**: `:SL#`/`:SC#`/`:SG#` accept a "local
  time + UTC offset" pair, but combining a nonzero offset with local time produced a further,
  not-cleanly-explained ~85-minute LST error on real hardware (not a clean multiple of the
  offset). Fixed pragmatically — an established pattern for LX200-family mounts — by always
  computing UTC via `std::gmtime()` and sending it as "local" time with `offset_hours=0.0`,
  sidestepping the firmware's own timezone/DST handling entirely. Verified via hardware:
  offset=0 + longitude=0 gave a GMST match within 6 seconds.
- **`:SG#` (UTC offset) parser is strict integer-hour ± optional fixed minutes**: accepts
  `sHH#` or `sHH:MM#` with `MM` restricted to `{00,30,45}` (`atoi2()`-based parser) — a decimal
  form like `"+01.0"` is rejected outright. `set_time()` builds whole-hour + nearest-of-{00,30,45}
  minutes rather than a naive `%+05.1f` format.
- **Bare-ack, no-`#`-terminator quirk on "set" commands**: `:Sr#`/`:Sd#`/`:MS#`/`:St#`/`:Sg#`/
  `:SL#`/`:SC#`/`:SG#` all reply with a bare `"0"`/`"1"` digit and **no trailing `#`** (firmware's
  `supress_frame=true`/`boolReply=false`), unlike the vast majority of LX200 commands. Every one
  of these call sites must use `send_command(cmd, /*require_hash_terminator=*/false,
  kSetAckTimeoutMs)` — using `require_hash_terminator=true` here (the initial, wrong
  implementation) hung every slew/site/time write until timeout and was the root cause of the
  first several ConformU failures (`SlewToCoordinatesAsync`, site/time writes).
  `kSetAckTimeoutMs = 1000` bounds the failure case only; real acks arrive in well under 100ms.
- **Serial `VTIME` must be 1 (100ms), not higher**: an earlier version copied Gemini's one-time
  probe `VTIME=20` (2s) into the production `connect_serial()` path, causing every blocking read
  to cost up to 2 real seconds — most visibly a ~2s stall on `PulseGuide` and pushing `AbortSlew`
  (5 sequential blind commands) toward ConformU's 1.0s STANDARD timeout. `send_command_blind()`'s
  stale-input drain window is `50ms` (one VTIME cycle) for the same reason — `120ms` (2+ cycles)
  pushed `AbortSlew` to 1.04s, just over budget.
- **`Park()`/`FindHome()` must be fire-and-forget, not blocking**: send the command and return
  immediately (optimistically marking `is_parked=false`/`is_at_home=false` and `is_slewing=true`,
  letting the next status poll pick up real completion), matching iOptron's pattern. A first
  version blocked synchronously until the mount physically finished (`wait_for_condition_locked`)
  — functionally correct, but ConformU classifies `Park`/`FindHome` under the strict STANDARD
  (1.0s) timing target when the call itself blocks for the whole operation, vs. the lenient
  EXTENDED (600s) target when the call returns fast and the *client* polls `AtPark`/`AtHome`
  afterward (confirmed by comparing timing-summary entries against iOptron's own passing
  ConformU log, where `Park`/`FindHome` also take ~30s in wall-clock terms but show `0.001s ✓
  (EXTENDED)` in the timing table). Real GEM mounts physically cannot park/home in 1 second;
  fire-and-forget is the only way to pass this target with genuine hardware.
- **`DeviceState`'s first post-Connect call routinely exceeds the 0.1s FAST target (~0.65s),
  and this is NOT fixable by warming caches during `connect()`**: two different warm-up
  strategies (locked, then a mutex-released ToupTek-AFW-style version) were tried against real
  hardware and neither moved the measured time at all — ConformU's Connect()/Connected preamble
  (`Connected=True` → `Connected=False` → `Connect()`) calls `DeviceState` fast enough after the
  final reconnect that the live `:GA#/:GZ#/:GU#/:GRa#/:GDe#` round trips always land inside its
  measurement window regardless of what connect-time work has or hasn't finished. Treat this as
  an accepted, inherent one-time cold-start cost, not a bug to keep chasing.
- **The same connect-adjacent cold-start window can land on other FAST members too, not just
  `DeviceState`** (re-confirmed on real hardware, ConformU 4.5.0, after the driver-branch update
  that ported this driver's auto-detect to the shared `serial_by_id_scan.h` helper): two
  consecutive runs both showed `AlignmentMode` (~0.135s), `Declination` (~0.318s),
  `EquatorialSystem` (~0.136s), and `SideOfPier Read` (~0.140s) OUTSIDE the 0.1s FAST target,
  every time, at nearly identical values (±0.003s) — including `AlignmentMode`/`EquatorialSystem`,
  which are compile-time constants with no wire I/O at all. This is the same phenomenon as the
  `DeviceState` note above, not a regression from that auto-detect change (which only touches
  `enumerate_onstep_ports()`, never called on this code path): manually repeating the same calls
  a few seconds after `Connect()` (well outside ConformU's rapid-fire post-Connect property sweep)
  answers in ~0.003s every time, confirming it's a transient connect-adjacent window, not a
  permanent per-call cost. Which members land inside that window depends on ConformU's exact call
  order/pacing for a given version, so don't be surprised if it isn't `DeviceState` specifically.
  Same conclusion applies: treat as accepted, not a bug to chase-fix via caching.
- **`PulseGuide` East/West shows 1-2 residual ConformU issues per run, right at the 0.07″
  tolerance boundary, and this is inherent hardware noise, not a driver bug**: root-caused via
  four independent tests, all on real hardware — (1) A/B test with OnStep's refraction
  rate-compensation feature (`:Tr#`/`:Tn#`) on vs. off: no correlation, disabling it made the
  residual *worse*; (2) mount unloaded vs. with the real telescope mounted and balanced: no
  measurable difference; (3) attempted testing far from the celestial pole: blocked by ConformU's
  own `Park()`/`FindHome()` always returning the mount near the pole regardless of test
  settings; (4) the same residual reappears at a different member (sometimes `-9.0`, sometimes
  `+9.0`, North or South) across repeated runs, and all 4 rate labels (`-9.0`/`-3.0`/`+3.0`/
  `+9.0`) drive the *same* physical pulse (`GuideRateDeclination`/`RightAscension` are fixed,
  `CanSetGuideRates=false`) — i.e. these are 4 repetitions of one experiment, and a noise floor
  straddling a tight tolerance will intermittently cross it. Do not attempt to "fix" this by
  loosening tolerances or by re-adding rate-compensation toggling.
- **v1 scope is equatorial mounts only** (`AlignmentMode::GermanPolar`), matching every other
  telescope driver in this project (iOptron, SynScan, Celestron). `SlewToAltAz`/
  `SlewToAltAzAsync` are still supported via a local Alt/Az→RA/Dec coordinate transform followed
  by an ordinary equatorial slew — this works regardless of the physical mount's mechanical
  alignment, so it does not require native AltAz mount support. `SyncToAltAz` is not implemented
  (`CanSyncAltAz=false`) since OnStep has no native Alt/Az sync command.
- `CanSetGuideRates=false` (guide rate is not queryable/settable over this protocol — the
  `GuideRate` property returns an informational fixed value only) and `CanSetPierSide=false`
  (no distinct "set pier side" command). `CanSetDeclinationRate`/`CanSetRightAscensionRate` are
  also `false` — custom tracking-rate commands were not confirmed against real OnStep firmware
  during this implementation, so the setters throw `PropertyNotImplemented` rather than
  advertise support that might silently no-op.
- **ConformU-validated on real hardware** (Generic OnStep DIY harmonic-drive mount, firmware
  "On-Step" v10.23a, Linux arm64, ConformU 4.4.0): 0 errors, 0 timing violations in the saved
  run. The only remaining findings are the 2 PulseGuide East/West residual issues documented
  above as an accepted, inherent hardware-noise limitation rather than a driver bug — earlier
  runs during this same validation session also hit the DeviceState-cold-start-style FAST
  timing risk described above (via `get_side_of_pier()`'s live position round trip, since
  fixed), which is why that failure mode is documented even though the saved log is clean of it.
  See `AlpacaCore/conformu/OnStep/Generic OnStep/Linux-arm64.txt` and `SUPPORTED-DRIVERS.md`.

### WiFi manager (AlpacaHTTP, 3.4.0)

- NM D-Bus property types matter: `ActiveConnection`, `Ip4Config`, and
  `ActiveAccessPoint` are object paths ("o"), not strings — reading them
  with `sd_bus_get_property_string` fails sd-bus's type check silently
  (empty result, no error), which presented as "hotspot shows off while
  broadcasting". Use a dedicated "o"-typed reader.
- Shared-mode (hotspot) activation needs polkit
  `org.freedesktop.NetworkManager.wifi.share.protected`/`.share.open` in
  addition to `network-control` — the failure ("Not authorized to share
  connections via wifi") only appears at ActivateConnection time and was
  found by live-testing the endpoint, not by review.
- NM `Update()` with a `802-11-wireless-security` section declaring
  `key-mgmt` but omitting `psk` PRESERVES the stored secret
  (hardware-verified on the Pi 5 rig). `GetSettings()` never returns
  secrets, so this cannot be confirmed from read-back — verify on hardware
  when in doubt.
- One `sd_bus*` connection is not thread-safe: every method serializes on
  one mutex, and that mutex must stay held across any waits between bus
  calls (a review round caught an unlock-during-sleep race). Blocking work
  that touches no bus state (nl80211 netlink) belongs outside that mutex,
  on its own serialization if ordering matters.
- Vendor driver capability reports lie: RK3568 `bcmdhd` tells NM 2.4-only
  while 5 GHz hotspots work; iMate `unisoc_wifi` returns nothing to
  unprivileged `iw` and rejects wpa_supplicant's WPA2 group-key install
  (hostapd's sequence works). Board matrix in `docs/wifi-manager-design.md`.
- 5 GHz AP init fails under the WORLD/00 regdom on some drivers — the
  persisted country must be applied at daemon startup (main.cpp), BEFORE
  NM's boot-time AP autoconnect, not lazily on first request.
- Even when 5 GHz AP init *succeeds* under WORLD/00, the AP can beacon on a
  world-domain-forbidden channel (seen live: NM auto picked ch 149 on the
  OPi 4 Pro) that clients refuse to see or join — "the network disappeared".
  `set_ap` therefore rejects band "a" until a country is persisted, and the
  web UI front-runs that with a message pointing at the country selector
  (3.5.1). 2.4 GHz is exempt: ch 1-11 are world-domain legal, which is why
  the shipped images default to 2.4 GHz ch 6.
- The review bot login is `github-actions`; every push restarts a full
  review round — batch fixes. Test rig persisted-device state makes
  `test_routing` fail with "already registered". Since #274 each of the two
  router-backed binaries runs in its own ctest `WORKING_DIRECTORY`, so the
  files to clear are `AlpacaHTTP/build/test_routing_cwd/config/` and
  `AlpacaHTTP/build/test_persisted_devices_cwd/config/`, not
  `build/config/`.

## General Notes

- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- ConformU logs live under `AlpacaCore/conformu/`.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add desktop GUI frameworks (Qt, GTK, wxWidgets, etc.). The web UI in `AlpacaHTTP/web/` is the only user interface.
- Do not invent device types outside the ASCOM Alpaca standard set (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope). Shutter control is part of the **Dome** interface (`OpenShutter`/`CloseShutter`/`ShutterStatus`), not a standalone device. A non-standard `Shutter` device type existed as unused scaffolding and was removed 2026-06-09 — clients (NINA, ConformU) cannot consume non-standard types, so they break interoperability.
