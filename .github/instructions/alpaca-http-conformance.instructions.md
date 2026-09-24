---
applyTo: "AlpacaHTTP/**"
---

## Alpaca Protocol Conformance (AlpacaHTTP)

Related decisions and failures — read when changing the behavior they explain:

- [Server-thread ownership decision](../../docs/decisions/0002-server-thread-ownership.md)
- [Failed-bind thread failure](../../docs/failures/0005-server-failed-bind-thread.md)
- [Release-build assertion failure](../../docs/failures/0004-ndebug-disabled-http-assertions.md)


These rules come straight from the ASCOM Alpaca API definition (https://ascom-standards.org/api/) and are enforced by ConformU. Do not regress them:

- **Parameter names are case-insensitive.** The spec: "Parameter names are not case sensitive, so clients and drivers should be prepared for parameter names to be supplied ... with any casing." This applies to **both** GET query params and PUT form-body params. `Request::get_query_param`/`has_query_param` and the router's `get_form_value` all match case-insensitively. Never special-case behavior on `User-Agent` (e.g. a "strict only for ConformU" path) — test behavior must equal production behavior.
- **URLs are case-sensitive and lowercase.** Device type and method path segments must be lower-case; that check stays.
- **HTTP status codes:**
  - `200` — request was interpreted and reached the driver. Driver exceptions (NotImplemented, InvalidValue, NotConnected, etc.) ride in the JSON `ErrorNumber`/`ErrorMessage` fields with a `200`. `apply_error_status` exists to keep these at 200 — never downgrade a driver error to 4xx/5xx.
  - `400` — "the device could not interpret the request e.g. an invalid device number or misspelt device type." Use 400 (not 404) for unknown device type, unknown method, a known method on a verb it does not accept (after the cross-origin 403), a device number outside 0..4294967295, and unregistered device number. A genuinely unroutable URL (no device/management match) stays 404.
  - `500` — unexpected internal error only.
- **The Alpaca `Value` is structured JSON, never a re-parsed string.** `AlpacaResponse::value` is `std::optional<nlohmann::json>` and `to_json` emits it verbatim. Handlers assign the real type directly — scalar, string, array, or object (e.g. `alpaca_response.value = actions;` for `SupportedActions`, **not** `actions.dump()`; `make_success_response(..., gains)` where `gains` is a `nlohmann::json` array). Do NOT serialize a structured payload to a string and rely on it being re-parsed downstream. The old `to_json` ran `json::parse()` on every string `Value` and substituted the result if it parsed — which (a) corrupted scalar string properties whose text is valid JSON (`"12345"` → number, `"true"` → bool, wrong ASCOM type on the wire) and (b) forced every array/object endpoint to round-trip through `.dump()`. That heuristic bit `SupportedActions`/`DeviceState` (every device) plus camera `Gains`/`Offsets`/`ReadoutModes`, telescope `AxisRates`, and filter `Names`/`FocusOffsets` — ConformU rejected the stringified arrays ("could not be converted to IList`<String>`"). The web UI mirror (`web/app.js parseResponseValue`) only parses a string that begins with `{`/`[`, never a bare scalar. The large camera image payload uses its own `build_image_*_payload` path and never goes through `Value`.
- **The router's `Connected=false` wait must poll `get_connecting()`, never
  `get_connected()`** (QHY ConformU session, 2026-07). The `PUT /connected`
  handler synchronously waits for an async disconnect to finish before
  replying. `get_connected()` is not a valid completion signal for that wait:
  per the [Handles/locks rule](../../AGENTS.md#driver-concurrency--lifecycle-read-before-writing-a-driver), `set_connected(false)` correctly clears
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
  wrapper-backed switch drivers (iMate PowerBox, StellaVita, ASIAIR, ASIAIR
  Plus) used to block too, inside the wrapper's `is_open()`; since issue #382
  each wrapper publishes its open state as an atomic written only inside its
  own `open()`/`close()` critical sections, `is_open()` is a lock-free read,
  and `check_docs_drift.py` classifies those drivers as lock-free only while
  that stays true. The rule applies to every driver; only the mutex-holding
  telescopes make it urgent. Every router
  site now reads `get_connecting()` first and short-circuits; while a task
  is in flight `Connected` reports false. A connect request that arrives
  mid-task is still passed to `device->connect()` so `AsyncConnectable` can
  queue it against an in-flight disconnect or drop it against an in-flight
  connect. Driver side, prefer an atomic `connected_` with a lock-free
  getter (every driver does except the ones `async_connectable.h` names --
  SynScan is among those that DO have a lock-free getter, since the #130 fix) —
  the telescopes it lists still take the mutex and rely on the router rule; the
  wrapper-backed switch list is empty since #382 and the gate keeps it so.
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
  majority) versus which aren't (the telescopes above).
  **Known gap (narrow, code review on PR #3):** `get_connecting()` and
  `get_connected()` are two separate calls, not one atomic snapshot — if a
  connect task starts in the gap between them, the `get_connected()` call
  can still block on a mutex-holding driver's handshake for the telescopes above.
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
  - **No server thread is ever detached on the self-join path.** A thread
    `stop()` cannot join because it is running on it (a handler calling
    `stop()` synchronously; no current handler does) goes into
    `orphaned_threads_`, and the next `stop()` from another thread or the
    destructor joins it. The threads this covers are the accept/server
    thread, the reactor, the worker pool and the RTC probe timer
    (`rtc_probe_thread_`, #314) -- the last spawns and joins alongside the
    reactor and takes no lock `stop()` holds. Since #547 the same thread also
    ticks the client-silence motion watchdog every second
    (`Router::run_motion_watchdogs`); it rides this thread rather than the
    reactor or a thread per device for the identical reason the RTC probe
    does -- its mount I/O (`Slewing`/`AbortSlew`) must never block `poll()`.
    So nothing can touch a
    `Server`'s members, the wake pipe included, after the destructor returns
    (review round 5). Destroying a `Server` from inside one of its own
    handlers is not supported.
    **Exception (issue #507, `join_or_abandon()`):** if `pthread_join` fails
    on any of these threads on the CROSS-thread path (`ESRCH`/`EINVAL` --
    `EDEADLK`, the self-join case, is excluded, since it never reaches
    `join_or_abandon()`), the thread IS detached rather than retried: a
    second `join()` on a handle that already failed once is not provably
    non-blocking (`docs/decisions/0002-server-thread-ownership.md`), so
    retrying it from `join_orphaned_threads()`, which runs under
    `lifecycle_mutex_`, risks a deadlock instead of a clean failure. This is
    a fault path, not normal operation -- neither the original PR nor its
    review could demonstrate it reachable through the current test harness
    -- but if it fires, the invariant above is void for that one thread: it
    can still touch `this` (the wake pipe included) after `~Server()`
    returns. `close_wake_pipe()` racing a detached worker's `wake_reactor()`
    is the concrete consequence; nothing currently guards it.
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
  - `Config::motion_watchdog_seconds` (open-astro#547; 30 s default, matching
    AlpacaCore's `kClientSilenceStopInterval` in `util/motion_policy.h`; 0
    disables) is settable the same way, from the config file (`server:`
    section) and the environment (`ALPACAHTTP_MOTION_WATCHDOG_SECONDS`),
    routed through its clamping setter (negative -> 0); tested in
    `test_config.cpp`.
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
