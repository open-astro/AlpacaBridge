---
applyTo: "AlpacaCore/src/**,AlpacaCore/include/**,AlpacaCore/tests/**"
---

## Driver concurrency and lifecycle

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

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
- **Operation task threads: use the slot where a driver has adopted it.** The Celestron
  (NexStar and CGX), SynScan, Sky-Watcher and iOptron telescope drivers, the Bisque telescope's pulse guide and the Gemini
  focuser and flat panel run their operation bodies (async slew, park, home, pulse-guide
  timer, slew-completion tail) in a `util::AsyncOperation` slot
  (`AlpacaCore/include/alpacacore/util/async_operation.h`, [decision 0006](../../docs/decisions/0006-async-operation-ownership.md)).
  The slot owns thread start, reaping of replaced bodies, cancelled-vs-superseded stop
  reasons, cancellable `ctx.wait_for()` polling, the kept failure and the join at
  destruction; it never detaches a thread. **A new driver must not hand-roll a task
  thread (no own `std::thread` member, no `reap_*`/`task_wait_for` helper): use the slot.**
  Call `start()` and `cancel_all_and_join()` without the driver `mutex_` held, join the
  slots in the destructor before members are destroyed, and never call `start()` or
  `cancel_all_and_join()` from a body of the same slot (the paragraph after rule 10 in the
  header comment). A body that calls anything throwing still catches it to reset the
  driver's own state (`slewing_cached_`, `parking_`) before the slot keeps the failure;
  an uncaught throw would leave those flags set. Bounding a body stuck in a no-timeout SDK
  call stays the caller's job (see the handle-reference-counting item below).
- **Threads that are not in a slot keep these rules.** Hand-rolled threads remain in the
  ZWO telescope (`goto_thread_`, `pulse_thread_`, `poll_thread_`, `disconnect_thread_`),
  the QHYCFW3 move thread,
  the camera exposure workers, the QHY cooler-off and temperature workers and the GPIO
  soft-PWM workers; the list is a sample, not a roster.
  - **Never `.detach()` a thread that touches `this`.** A `sleep_for` timer that later
    writes a member is the classic trap: if the object dies mid-sleep the wakeup writes
    freed memory (UB). Make it a joinable member thread with a cancel flag +
    `std::condition_variable`, and cancel + join it in the destructor. The one
    sanctioned exception is the bounded detach-on-timeout worker below. The 2026-07
    full-codebase audit found this rule held on every connect/disconnect *lifecycle*
    path but violated on a dozen *operational* paths; apply it to every thread a driver
    spawns, not just the connection machinery.
  - An async tail that calls anything throwing (e.g. a `*_locked()` helper that rethrows
    `NotConnected`) must be wrapped in try/catch inside the lambda or it
    `std::terminate`s the whole server.
  - A `joinable()` pre-check before spawning a shared member thread is a double-start
    race unless the check, spawn and assign run under one mutex.
  - The destructor joins **every** background thread that is not in a slot (connection,
    exposure, timers) before members are destroyed.
- **Serialize reapers across the whole join.** `AsyncOperation::cancel_all_and_join()`
  takes its body out of the slot before joining; a second caller can see an empty
  slot while the first still owns a running worker. A driver-level lifecycle/handoff
  lock must cover that entire join and prevent teardown from closing its handle.
  Background bodies must not acquire the lock their joiner holds. Concrete failure:
  [CGX handoff/disconnect race](../../docs/failures/0011-cgx-handoff-disconnect-race.md).

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
  exactly the snapshot-then-call gap. Prefer a `with_handle([&](auto h){ return
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
- Every persisted field declared per device type in the vendor's catalog schema
  (`<vendor>_fields.h`; `sanitize_device_config` drops undeclared keys), every
  non-ZWO form field `name` vendor-prefixed — the full rules live in ONE place:
  [Enumeration index fields](http-integration.instructions.md#enumeration-index-fields--unique-names--auto-numbering-all-vendors).
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

### When fixing a review finding (avoid the regression treadmill)

Related failure: [PR #99 sibling-fix misses](../../docs/failures/0003-review-sibling-fixes.md).

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
  stays true while the fault is young (past `util::kLinkStalenessBound` a driver that adopted [decision 0009](../../docs/decisions/0009-link-loss-and-relink-policy.md) drops it) so `NotConnected` would contradict it.
  Where ASCOM has a word for "unknown" (`CoverState`/`CalibratorState::Unknown`) return it
  instead of throwing on the read; commands still throw.
- **Static metadata keeps answering** (names, descriptions, ranges, `CanWrite`, driver-side
  filter names/offsets) — it does not depend on the device. `DeviceState` then degrades to
  `TimeStamp` only through the base class's per-id try/catch.
- **Recovery is automatic**: keep polling/reading at the normal cadence while faulted so the
  first frame clears the latch without a reconnect (a re-plugged hub on the same node).
- **Test it hardware-free** with the pty fakes: `set_muted(true)` (hung MCU, healthy fd) and
  `sever_link()` (master closed: writes fail with EIO, reads return 0 at once) — `tests/fake_serial_streamer.h` for any
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
  lock-free `link_lost_` latch the getter reads; since #1310 that transaction is the state poll
  (250 ms), because the getters are served from a snapshot and make none. OnStep follows the Sky-Watcher shape since #1241
  (`OnStepProtocolWrapper::link_alive()`; pty tests in `test_onstep_link_loss.cpp`). The #237 drivers still treat a
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
