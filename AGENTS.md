# Agent Instructions

This file owns the shared agent rules for this repository; path-specific rules
live in `.github/instructions/`.

Resolved incident records live in `docs/failures/`; design rationale lives in
`docs/decisions/`. Keep current rules here and in scoped instruction files, with
short pointers to those records rather than copying their history into new rules.

Domain terms that are easy to confuse (device number vs enumeration index, API vs
persisted config, cancelled vs superseded, Connected vs link health, fake vs rig
evidence) are defined in [CONTEXT.md](CONTEXT.md); module names are in
[the architecture overview](docs/architecture.md#modules).

Issues live in GitHub Issues on `open-astro/AlpacaBridge`, via the `gh` CLI —
see `docs/agents/issue-tracker.md` for conventions.

## Load the complete instructions before working

Read this entire file before planning, reviewing, or editing, even if the client
has automatically supplied only its beginning. Read long files in bounded chunks
until the end; a truncated tool result does not count as a complete read.
Then read [the instruction index](docs/agent-instructions.md) and every scoped
instruction file relevant to the task, including vendors mentioned in the request
or affected through shared routing, configuration, SDKs, or tests. Follow relevant
cross-references and memory records. Revisit the index when the task expands.
These are required reads even when the client does not interpret `applyTo`.

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
  `record_pending_disconnect()`. **A driver whose `set_connected(true)` is itself slow
  (a serial handshake, an HTTP fetch, a GPIO request) must also hold a driver-level
  `transition_mutex_` across the whole of `set_connected()`** (issue #528): the base gates
  only see an *async* connect, so a sync disconnect landing inside a sync connect saw
  "not connected" twice and returned as a no-op while the connect stored true. Guard
  `set_connected()` against `set_connected()` only; never take it in a getter.
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

Related failure: [PR #99 sibling-fix misses](docs/failures/0003-review-sibling-fixes.md).

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
| `ActionNotImplemented` | `Action()` called with a name the driver does not list in `SupportedActions` (0x40C, ASCOM's `ActionNotImplementedException`). An unsupported `CommandBlind`/`CommandBool`/`CommandString` is `MethodNotImplemented`, not this. |
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
- **Exception: a removed device node is a lost connection, not a fault (issue #445,
  Sky-Watcher direct driver).** A quiet link can come back on the same fd; a node that is gone
  cannot (the fd's link count is 0, writes fail `EIO`, and the held fd keeps the kernel from
  reusing `/dev/ttyUSB0`, so the replugged adapter enumerates as `ttyUSB1`). There
  `get_connected()` asks `SkyWatcherProtocolWrapper::link_alive()`, which compares the
  configured path's `stat()` with the node opened at connect (no I/O, never waits on an
  exchange) and closes the dead fd when it can; operations throw `NotConnected`; and a
  `Connected=true` against the lost link reconnects instead of hitting the idempotency return.
  `EIO`, `ENXIO`, `ENODEV` or `EBADF` from a write or read on the link's fd also counts as loss,
  even before the node lookup reflects it: a tty returns those only when its device is gone or
  the fd is unusable. Silence with the node still present keeps the rules above. The QHY Q-Focuser follows the same
  shape since #527, detected on its next transaction (request/response, no reader thread) with a
  lock-free `link_lost_` latch the getter reads. The #237 drivers still treat a
  removed node as a fault; that has not been changed. Tests: `sever_link()` on
  `fake_skywatcher_serial_board.h`, `[skywatcher][serial][connected]`.

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

### Arduino-class serial devices reset when the port opens (QHYCFW3)

A USB-serial bridge (CP2102, CH340, FTDI) asserts DTR on `open()`, and an
Arduino-class MCU behind it (ATmega328P with the auto-reset capacitor) reboots
on that edge. The QHYCFW3 then homes for ~17 s and emits one status byte;
everything written before that byte is discarded, and clearing HUPCL before
`close()` did not stop the next open from resetting it. Symptoms on the bench
read exactly like a protocol bug: motion on every open, a single stray byte
that arrives a fixed time after the open regardless of what was sent, no reply
to any query. Rules: (1) before assuming a dead protocol, log the time of each
inbound byte relative to the OPEN, not the last command; a constant offset is a
boot, not a reply; (2) the wrapper's `connect()` waits for the boot byte with
a bounded timeout and proceeds at once when it lands (`Cfw3ProtocolWrapper`
is the reference), the sync `Connected=true` path holds a
`transition_mutex_` because the connect is now multi-second (#528), and the
wrapper HOLDS the fd across logical disconnects so a reconnect never
re-opens the port: **ConformU abandons a Platform 7 `Connect()` after 5 s of
`Connecting`** ("The Connecting to device operation exceeded its 5 second
timeout", CFW3 run 1), so any connect that costs more than that on every call
fails the suite outright, and a boot paid once per process and never again
is the only shape that passes; (3) an
auto-detect probe of such a device resets everything on the same chip class,
so filter candidates by descriptor, warn in the log, and recommend an
explicit port in the UI; (4) a device with a hardware control-mode switch
(the CFW3's USB vs 4-pin button) can boot, home and emit its status on USB
while ignoring every USB command, so put the switch in the connect refusal
message and check it before debugging the parser.

### Auto-detect failure message (`util/auto_detect.h`)

Serial port enumeration is POSIX-only, so the `enumerate_*_ports()` helpers return
empty on Windows. When an auto-detect driver finds no ports, throw
`util::serial_auto_detect_failed_message("<device label>")` rather than a
hard-coded "no device found" string, so the Windows path reports "auto-detect not
supported on this platform" instead of implying missing hardware.

### Auto-detect resolves at connect, never in a factory (`util/connection_resolver.h`)

A persisted device is constructed at server start-up, and the hardware is often
not there yet: a Wi-Fi mount is still joining the access point, a USB adapter is
powered after the SBC. Every auto-detect factory (`create_*_auto`,
`_auto_network`, `_by_index`) used to run its serial probe or subnet sweep at
construction, so the factory threw, the router logged "Failed to load persisted
device", the web UI showed `(failed to load)`, and nothing ever retried (issue
#659, iOptron HAE over Wi-Fi on the Pi rig). Rules:

- **The factory hands the driver a `util::ConnectionResolver<Info>`** (the
  scan as a callable that returns the endpoint or throws the operator-facing
  refusal) and constructs without touching the bus. The scan body stays a
  plain function (`resolve_<vendor>_<mode>(index)`) so it is reusable and
  testable on its own.
- **The connect path calls `util::connect_resolved(info_, resolved_, resolver_,
  try_connect)`** under whatever lock it already holds. `try_connect` throws on
  failure (the existing `if (!protocol.connect(info)) throw ...` moved into the
  lambda). The helper retries the last resolved endpoint before scanning again,
  so repeated ConformU / NINA connects stay under the Platform 7 5 s `Connect()`
  budget. **Re-scanning is opt-in**: the retry falls through to the resolver
  only when the lambda throws `util::StaleEndpoint`, which it does for a
  vanished serial or HID node (`util::device_node_missing()`), a refused network
  connect, or a failed identity gate (SynScan's echo test). Every other
  exception propagates: a scan DTR-resets every CP210x / CH340 device on the
  box, so a wheel still homing or a handshake that missed once must never start
  one (review of #660). The scan's own message propagates as the connect
  refusal (#358), so it reaches the client and `LastConnectError`. **The first
  connect after a service restart pays the scan** (5.5 s for the iOptron Wi-Fi
  sweep, up to two boots for the CFW3 probe) inside the Platform 7 `Connecting`
  window, so before a ConformU run against a freshly restarted service connect
  the device once from the web UI, or give it an explicit port or host.
- **Every driver converted from a construction-time scan (the ten in #660)
  exposes a `create_*_deferred(device_number, resolver, ...)` seam** and ships
  the cases in `tests/deferred_connect_cases.h` over its fake:
  refused (construction succeeds, the refusal is the connect error, sync and
  async), reused (one scan across a reconnect), re-resolved (the fake behind the
  resolved endpoint dies, a new one appears, the driver reaches it). A driver
  with no fake (Astroasis, hidapi) ships the refusal case alone and says so.
  Put the vendor's cheap identity gate (SynScan's echo test) INSIDE the
  `try_connect` lambda: the helper only re-scans when the lambda throws, and a
  stale port another adapter now owns still opens.
- Drivers that resolve inside `set_connected(true)` by hand (iEFW
  `resolve_serial_port_locked()`, Gemini PDH / flat panel, WandererAstro) already
  meet the rule; do not move them back into a factory. A new `_auto` or
  `_by_index` factory that scans at construction is a review-blocking regression.

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
  Telescope 4, Focuser 4, Rotator 4, FilterWheel 3, Switch 3, CoverCalibrator 2 (ICoverCalibratorV2),
  ObservingConditions 2.
  Keep `get_interface_version()` and its unit-test assertion in sync when adding a driver.
- **Do not** write a per-vendor `get_device_state()`. Each device base class
  (`CameraDriver`, `TelescopeDriver`, …) implements it once, inline, building the
  operational-property list by calling that device's own property getters inside a
  `try { … } catch (const std::exception&) {}` (a getter that throws — `AlpacaException`
  or any unwrapped vendor error — is omitted, never propagated) and
  appending a `TimeStamp` via the inline `device_state_timestamp()` helper. A disconnected
  driver returns the empty list with no `TimeStamp` (each base class checks `get_connected()`
  first). Using the
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
- To cut a release, run `/bump-release` (`.claude/commands/bump-release.md`): it bumps the `VERSION` file, dates the `## [X.Y.Z]` CHANGELOG.md heading, updates the README badge and device count, writes the plain-language notes in `docs/releases/<version>.md` that become the GitHub Release body, and tags the merge — **do NOT edit `debian/changelog`; it is generated** (see the packaging note above).

### Version bump policy (SemVer)

`VERSION` and the `## [X.Y.Z] - UNRELEASED` CHANGELOG heading move together, per
SemVer: **new driver/feature = minor bump; fix- or docs-only = patch; breaking change
(dropped platform, config-schema break) = major.** The UNRELEASED section carries
forward cumulatively until release — if it already sits at a minor bump and another
driver lands, the number stays; a feature landing on a patch-level UNRELEASED raises
it to the next minor. `/commit` and `/submit-pr` enforce this; it is documented here
so a driver-building agent bumps correctly without them.

## Testing Requirements

Related failure: [Release builds disabled HTTP assertions](docs/failures/0004-ndebug-disabled-http-assertions.md).

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

### Hardware-free driver tests via the SDK seam (ToupTek, QHY and gphoto — extend to other vendors)

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

### Test CMake Integration

When adding a test file for a new vendor device:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list in `AlpacaCore/tests/CMakeLists.txt`, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.
- Build and run all tests (`cmake -B build-vendors -DALPACACORE_ENABLE_<VENDOR>=ON && cmake --build build-vendors --target alpacacore_tests && ./build-vendors/tests/alpacacore_tests`, from `AlpacaCore/`) before considering the driver complete. Use `build-vendors`, not `build`: since #588 a default pre-flight leaves the `build` directory under AlpacaCore/ holding a sanitized, vendors-OFF configure, and CMake caches `CMAKE_CXX_FLAGS` from the first configure of a directory, so reusing it compiles no driver and stays sanitized whatever you pass.

## Continuous Integration and Pre-flight

Related decision: [Documentation drift gates](docs/decisions/0003-docs-drift-gates.md).

- CI (`.github/workflows/ci.yml`) runs on every PR, all on the native arm64 runner: `build-test` (vendors OFF) + `build-vendors` (vendors ON), `sanitizers` (ASan+UBSan), `sanitizers-tsan` (ThreadSanitizer over the `[stress]` connect/disconnect/operate concurrency suite, all vendors ON, plus `[stress-guard]` for the harness's own self-tests), `clang-format`, `clang-tidy`, `cppcheck`, `unicode`, `shellcheck`, `javascript`, and `zizmor`.
- **Layering gate** (`scripts/check_layering.py`, the `layering` CI job and pre-flight gate 2f, issue #651): counts `#include <alpacacore/vendor/...>` lines under `AlpacaHTTP/` and in the catalog schema files, and fails when either exceeds the baseline held in the script (`MAX_ALPACAHTTP_VENDOR_INCLUDES`, `MAX_CATALOG_SCHEMA_VENDOR_INCLUDES`). Each vendor descriptor slice of [device-catalog](docs/decisions/0004-device-catalog.md) lowers the AlpacaHTTP constant in the PR that deletes the includes; the baseline never rises.
- **Contract sweep** (`AlpacaCore/tests/contract_sweep.h` + `test_contract_sweep.cpp`, issue #571): every (vendor, device type) pair `Router::register_device_from_config()` constructs has one registry entry, and the sweep runs the tier-1 ASCOM contract cases over each disconnected, one ctest case per (driver, case) named `Contract sweep - <vendor>_<type> - <case>`. A driver that differs states the expectation in its registry entry with a source (protocol document, hardware run, or assumption); a new driver gets an entry, not a copy of the assertions. `scripts/check_contract_sweep.py` (the `contract-sweep` CI job and pre-flight gate 2g) fails on a router pair with no entry and no `ALLOWLIST` reason, a stale allow-list entry, and a guard the tests CMake does not define for `alpacacore_tests` (the `ALPACACORE_ENABLE_<VENDOR>` macros are not inherited from the vendor targets, so without the definition the registry compiles empty and the sweep passes vacuously). The docs-drift script pins the fake-connectable roster in the same header to the `fake_*.h` files on disk.
- **Every job in every workflow carries a `timeout-minutes` bound** (issue #363). The numbers are sized from the observed healthy runtime of recent green runs with wide headroom (`sanitizers-tsan` 30 min against a healthy max of 5, `build-vendors`/`sanitizers` 25, `build-test` 20, `clang-tidy` 25 -- it does the all-vendors build `build-vendors` does plus libgpiod from source and `clang-tidy-diff` over every changed line, so it gets that job's bound rather than a smaller one -- `cppcheck` 20 -- its dominant cost is building cppcheck 2.17 from source on the runner with no cache, so it gets the same bound as the build jobs rather than a text-scan-sized one -- the text scans 10; `release` 10, `codeql` 30, `claude-review` 45). They exist because the `[stress]` suite is the one place a regression can *hang* rather than fail, and GitHub's 6-hour default turned that into six hours of runner time before any signal. **When you add a job, give it a bound**, and when a job legitimately outgrows its bound raise the number rather than trimming the work to fit -- these are a backstop against a wedge, not a performance target. Note the deliberate gap on `claude-review`: its 45 min bound is longer than `/pr-checker`'s 30 min verdict-poll budget, so a review running past 30 min times the skill out while CI still lets the job finish. That is the intended precedence (the bound exists to catch a wedged job, not to pace the reviewer); a poll timeout is a re-poll, not a broken workflow.
- **Run `scripts/ci_preflight.sh` before opening a PR** (it is the `/submit-pr` Step 4 hard gate). It reproduces the CI gates locally, auto-installing missing tools, and exits non-zero if any mandatory gate fails — catching failures before they ever reach CI.
- **The ASan+UBSan pass runs by default in pre-flight** (issue #588); `RUN_SANITIZERS=0` skips it and reports `[SKIP]`, which is the right call for docs/CI-only work but not much else — it was on by default precisely because it had been the one configuration neither CI nor a contributor ran, and a deterministic 20/20 failure sat in the tree undetected as a result (`RUN_TSAN` is separate and still opt-in). This means a new class of local failure: a sanitizer finding in code the ordinary build passes. **When the sanitizer cannot observe a case rather than having found a defect in it, gate the case out of sanitized builds — do not loosen the assertion**, which would silently cost coverage in `build-test` and `build-vendors` too. The worked example is the `ALPACACORE_TESTS_SANITIZED` guard in `AlpacaCore/tests/test_fake_pty_write.cpp`, whose EMFILE case exhausts the process's descriptors so UBSan cannot obtain one for its vptr check; it returns early with a `WARN` under sanitizers and runs for real everywhere else. A genuine finding, by contrast, is a bug — fix it.
- **cppcheck is pinned to 2.17.x, built from source in CI.** The `ubuntu-24.04-arm` runner's apt cppcheck is 2.13, which classifies some checks differently from the 2.17 on a Debian Trixie dev box (e.g. `virtualCallInConstructor` is a `warning` in 2.13 but reclassified in 2.17). Since `ci_preflight.sh` runs whatever cppcheck the dev box has, that version skew let the local pre-flight and CI disagree. Building 2.17 from source (checksum-verified, mirroring the libgpiod-from-source step) keeps them aligned. **Keep the cppcheck `--suppress` list identical between `ci.yml` and `ci_preflight.sh`** — `scripts/check_docs_drift.py` (the `docs-drift` CI job / pre-flight gate) now fails if they diverge, so this can't silently drift again.
- **Web UI JavaScript is gated by `node --check` AND `node --test`** (the `javascript` job + pre-flight gate). The web UI is hand-written static JS served as-is, with no bundler and no `package.json`, so the syntax check was the only gate for a long time — and a syntax check catches a missing brace and nothing else. That was defensible while `app.js` was DOM wiring; it stopped being defensible when the file grew pure functions with a real contract (issue #385). **Pure helpers go in `AlpacaHTTP/web/format.js`, not `app.js`**: that file touches no DOM and no `app.js` global, `index.html` loads it first, and it ends with a `typeof module !== 'undefined'` export block that browsers ignore, so `AlpacaHTTP/tests/web/*.test.js` can `require` it from Node with no browser stub. `node --test` ships with the Node CI already installs, so there is nothing to add to the toolchain. A helper that reaches for `document` belongs in `app.js` and stays untested — keep the split honest rather than growing a DOM stub. The first cases are the by-hand verification table from PR #359 made executable (several `TZ` settings, local midnight, and `Intl.DateTimeFormat` patched to throw, to return incomplete parts, and to answer in 12-hour form); each was mutation-verified against the guard it covers, and the 12-hour case is the one that matters most, because it renders a plausible-looking WRONG time rather than an obvious failure.
- `zizmor`'s pinned version + sha256 appear in both `ci.yml` and `ci_preflight.sh` — bump them together; `scripts/check_docs_drift.py` fails the build if they disagree. The same script also fails if a `docs/development.md` build-options table row goes missing for a CMake `ALPACACORE_ENABLE_*` option, if `VERSION` and the README badge disagree, if AGENTS.md, `CONTEXT.md`, a scoped instruction file under `.github/instructions/`, an agent-skills doc under `docs/agents/`, a Claude skill doc under `.claude/skills/`, or a memory record under `docs/failures/` or `docs/decisions/` references a repo path that doesn't exist, if a first-party source comment names a memory record that doesn't exist, if the QHYSDK seam's interface / `LockedQHYSDK` / sweep lists disagree (issue #394), if the `sanitizers-tsan` job's filtered runs or their zero-test greps differ from the ones `ci_preflight.sh` spells out (issue #341), if a first-party source file under the `AlpacaCore/` or `AlpacaHTTP/` src, include, tests or examples tree does not carry the current AGPL-3.0-or-later header block starting within its first 25 lines, naming the pre-#113 long form when that is what it finds (issue #450), or if a backticked repo path in the Cursor rule files (`AlpacaCore/.cursor/rules/`, `AlpacaHTTP/.cursor/rules/`) or in `AlpacaCore/external/README.md` does not exist, resolved against the component the file lives under (issue #457), if the LF-normalized SHA-256 of `docs/AlpacaDeviceAPI_v1.yaml` no longer matches the snapshot pinned in `.claude/skills/ascom-alpaca-protocol/references/version-and-sources.md` (the skill's endpoint catalog was generated from that snapshot, and `/driver-build` Step 0 refreshes the schema from upstream), or if `async_connectable.h`'s blocking-`get_connected()` lists drift from the drivers (issue #381): it classifies every `get_connected()` override under `AlpacaCore/src/vendors/` by its body and fails on a blocking driver missing from a list, a lock-free one still named in it, or a count of either list stated anywhere the globs reach. It also fails if any `std::regex` in `AlpacaHTTP/src/http/router.cpp` is not declared `static` (issue #657: a per-request regex build cost a device-path request 14x a management request), or if a model in `SUPPORTED-DRIVERS.md`'s GPhoto table is not named in the STATUS paragraph of `.github/instructions/gphoto.instructions.md`; that gate is one-directional and covers only rows whose Connection cell starts with `USB`.
- **Concurrency now has automated coverage — but only where a driver is registered with the stress harness.** The `sanitizers-tsan` job (issue #101) builds all-vendors with ThreadSanitizer and runs the `[stress]` connect/disconnect/operate suite (`AlpacaCore/tests/concurrency_stress.h`): lifecycle storms from N threads, destruction racing an in-flight connect, and the racing-disconnect-never-dropped settle check. Locally: `RUN_TSAN=1 ./scripts/ci_preflight.sh`. Registered so far: ToupTek camera / AFW / AAF focuser / thermal switch (over the fake SDK seam, wrapped in `LockedToupTekSDK`), ZWO EFW + camera + EAF focuser + CAA rotator + dew heater switch, Player One Phoenix + camera + thermal switch, SVBONY camera, Bisque, OnStep, and — over the loopback fake-mount TCP seam (`tests/fake_mount_server.h`, which drives drivers into the *connected* state so the poll/pulse/GOTO/teardown threads actually run) — the ZWO, Celestron, SynScan, and iOptron telescopes, plus the SkyWatcher telescope over its own loopback UDP simulator (`tests/fake_skywatcher_mount.h`), plus (fail-fast, no fake seam) the iOptron iEFW filter wheel, iEAF focuser, and iMate PowerBox Switch, and the Astroasis Oasis focuser (hidapi, no fake seam exists), plus the WandererAstro cover calibrator, filter wheel and box switch over the pty streamer fake (`tests/fake_serial_streamer.h`) with the request/response rotator fail-fast, plus the Gemini PDH Advanced 3 Switch and Flat Panel Pro CoverCalibrator over their pty-backed fakes (`tests/fake_gemini_pdh.h`, `tests/fake_gemini_flatpanel.h`) with the Gemini focuser fail-fast, and the WeeWX ObservingConditions driver over an unreachable URL. **When you add or substantially change a driver, add a `[stress]` TEST_CASE for it** — one factory + one operate callback (see `test_touptek_concurrency_stress.cpp` for the overall shape; since #326 every one of the 15 merged registrations uses the guard on every call, so any of them is a correct example, and none defines a local `call()` helper -- the gate rejects one outright). Wrap each call in the operate callback with `alpacacore::test::StressCallGuard` (`concurrency_stress.h`, issue #322) rather than a local `try/catch` — `run_lifecycle_stress` wraps the WHOLE callback in one try/catch, not each call inside it, so on a fail-fast path a single throw silently skips every call after it unless each one is guarded individually. `StressCallGuard` samples **one line per distinct failure mode with its occurrence count**, not the first N events (#377) — the old cap let one thread faulting in a tight loop fill every slot with copies of one message before another thread recorded once, which is worst in a *connected* registration where a live driver throws often and the single distinct failure that mattered is the one crowded out. It swallows the expected `NotConnected` by default and counts everything else it catches (a non-`std::exception` throw is never caught by anything here and still `std::terminate`s the binary, exactly as it would without the guard), so the case ends with `INFO(guard.report());`, then `CHECK(guard.unexpected_count() == 0)`, then `CHECK(guard.total_calls() > 0)` — **all three lines, always**: only the first `CHECK` turns counting into a failure, and a file that wraps every call correctly and omits it passes whatever the storm throws while *looking* like it follows the pattern (#379); the `INFO` is what makes a failure legible, since without it the `CHECK` reports only the expansion (`3 == 0`) and names nothing it swallowed; and the `total_calls()` line is what stops the zero-check passing **vacuously** (#334), since a guard that was never invoked reports zero unexpected throws exactly like one that saw a hundred clean calls, so a storm that silently stopped exercising the driver — a renamed method, a guard clause that now short-circuits, a device target that quietly stopped building — would still report a passing run. `check_stress_registration.py` enforces all three lines, plus the guard's presence itself (its `GUARD_ALLOWLIST` is empty since #326 -- a new registration that skips the guard is a gate failure, not an allow-list entry, unless it says why) and rejects a local `call()` helper outright, that being the hand-rolled try/catch the guard replaced — this replaces re-deciding the guard's catch type per file, which flip-flopped across review rounds before #322. Its constructor argument **REPLACES** the default set rather than adding to it — pass `{NotConnected, PropertyNotImplemented}`, not just `{PropertyNotImplemented}`, or every racing-disconnect throw in the storm is counted as a regression and the case fails nondeterministically. **A registration that runs *connected* (over `fake_mount_server.h`, `fake_skywatcher_mount.h`, or any full-seam fake) must widen the expected set explicitly this way** — `NotConnected` alone fits a never-connected, fail-fast path, but an operate callback exercising a live driver can legitimately hit `InvalidValue`, `MethodNotImplemented`, or `InvalidWhileParked` (a slew racing a park) from ordinary calls too — remember `NotImplemented`/`PropertyNotImplemented`/`MethodNotImplemented` all share one numeric code (see the class doc), so they're not separately distinguishable inside the expected set. Drivers without a registration are still covered only by code review against the [concurrency checklist](#driver-concurrency--lifecycle-read-before-writing-a-driver); do not assume green CI means thread-safe for them. **The QHY SDK seam's three parallel lists are gated** (#394): `scripts/check_docs_drift.py` compares `QHYSDK`'s pure virtuals, `LockedQHYSDK`'s overrides and the forward-sweep method list in `test_qhy_fake_sdk.cpp`, and separately fails on a forward that does not go through `locked()`, with ONE named exception: `cancel_exposure()` must take its own `cancel_mutex_` and must NOT go through `locked()` (#339), because production's cancel deliberately skips the per-handle call mutex so it can interrupt a download already blocked on the same handle, and routing it through the shared mutex would queue it behind the very call it exists to interrupt — do not "fix" it back under `mutex_`; the gate rejects that shape. The compiler forces a forward to EXIST (an unimplemented pure virtual leaves the decorator abstract) but never that it takes the mutex, which is the only reason the decorator exists — an unlocked forward makes the fake racy under a storm and produces a TSan report naming the *fake*, the exact confusion the decorator was built to prevent. Model any future SDK decorator the same way. Rule (a) above has one sanctioned in-tree escape hatch: `FakeQHYSDK::before_call`, a hook that is null in every ordinary test and that a case sets deliberately to make one named fake method block (the three #339 cases use it to prove the slowest-forward watchdog, the cancel forward's own timing and the cancel overtake); a case that sets it owns the consequences and must release the block before its driver is destroyed. `scripts/check_stress_registration.py` (the `stress-registration` CI job / pre-flight gate) fails on any vendor/device-type pair that is neither registered nor explicitly allow-listed there, so the currently-unregistered drivers are tracked in one place instead of only in this paragraph. **Name a registration file `<something>_concurrency_stress.cpp` and add it inside the vendor's `if(TARGET alpacacore_<vendor>)` block in `AlpacaCore/tests/CMakeLists.txt`** — the gate enforces both halves: the file glob carries no `test_` prefix (issue #376: the prefix used to be load-bearing, so a file following the documented naming was rejected as a stray `[stress]` case), and a registration file listed in the unconditional `TEST_SOURCES` block is a failure (issue #396), because rule 3 below assumes every `[stress]` case compiles conditionally. That covers a file listed *both* ways too, since CMake de-duplicates the repeated source and the ungated mention is the one that decides, and `if(NOT TARGET ...)` does not count as gating, because it compiles the file exactly when the vendor is absent — one that does not re-satisfies the TSan zero-test grep with no vendor coverage at all, which is the exact failure that grep exists to catch, reached from the other direction. The script's C++ scans strip comments first (issue #386), so an illustrative case macro in a doc comment is documentation and not a registration — `concurrency_stress.h` carries one as the live proof — but **string literals are not stripped**, so keep examples in comments; and they cover every Catch2 registration macro (`SCENARIO`, the `TEMPLATE_*` family, the `_METHOD` fixture variants, `METHOD_AS_TEST_CASE`, `TEST_CASE_PERSISTENT_FIXTURE`, `REGISTER_TEST_CASE`) -- if Catch2 ever grows another, add it to `CATCH_TEST_MACROS`, since a macro missing from that tuple is invisible to the stray-`[stress]` rule and join adjacent tag literals the way the preprocessor does, so `"[vendor][camera]" "[stress]"` is three tags rather than two (issue #393). It separately rejects two tag mistakes per TEST_CASE, each scoped to where that mistake actually costs something: a `[stress-guard]`-without-`[stress]` case inside a `*_concurrency_stress.cpp` file (that tag is for harness self-tests only — a registration wearing it would still get TSan and still read as registered while dropping out of the vendor-coverage count), and a `[stress]` case anywhere else under `AlpacaCore/tests/` (a file outside that glob can compile unconditionally, in which case one such case alone satisfies the TSan job's vendor zero-coverage grep and makes it vacuous — this actually happened, see `test_async_connectable.cpp`). Note the first rule is deliberately limited to registration files: `[stress-guard]` is legitimate anywhere else, which is the whole point of the tag — a vendor case parked outside that glob wearing it would read as registered to a human without counting toward vendor coverage, but that is a naming problem rather than a tag one, and the gate does not try to catch it. Note also that the gate keys on vendor/device-type pairs, so a registered driver masks every other driver of the same vendor and type. Masked pairs today: the two ZWO ASIAIR switch drivers behind the ZWO dew-heater switch registration, the Gemini Cover Lite class plus the Rev2 model path behind the Flat Panel Pro registration, and the integrated QHY CFW driver (camera handle) behind the standalone QHYCFW3 USB registration. All are covered by code review only (the QHY one also by its fake-SDK unit cases). **`scripts/check_stress_registration.py`'s docstring is the authoritative list** — this sentence is a pointer to it, not a second copy, because it has already gone stale once by naming only ZWO. **SDK-callback paths especially**: the TSan suppressions mute any report with a vendor-blob frame on the stack, so a race in driver code invoked from an SDK internal thread is invisible to CI unless that callback path is exercised through a fake-SDK seam (fully instrumented, no suppression applies) — when you add an SDK callback to a driver, register a fake-seam stress path for it in the same change.


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

Vendor-specific guidance lives in `.github/instructions/<vendor>.instructions.md`, scoped to each vendor’s implementation and tests. Rules shared by multiple vendors belong in this file; do not duplicate them in vendor instructions. AlpacaHTTP protocol guidance lives in `.github/instructions/alpaca-http-conformance.instructions.md`.

## General Notes

Historical evidence: [July 2026 code audit](docs/failures/2026-07-11-code-audit.md). Read it as a historical snapshot, not current unresolved findings.

- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- ConformU logs live under `AlpacaCore/conformu/`.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add desktop GUI frameworks (Qt, GTK, wxWidgets, etc.). The web UI in `AlpacaHTTP/web/` is the only user interface.
- Do not invent device types outside the ASCOM Alpaca standard set (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope). Shutter control is part of the **Dome** interface (`OpenShutter`/`CloseShutter`/`ShutterStatus`), not a standalone device. A non-standard `Shutter` device type existed as unused scaffolding and was removed 2026-06-09 — clients (NINA, ConformU) cannot consume non-standard types, so they break interoperability.
