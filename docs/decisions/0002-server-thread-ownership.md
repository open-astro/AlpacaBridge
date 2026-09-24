# HTTP server thread ownership

Status: accepted

## Context

`run_server()` may return before the server is running, including when `bind()` fails. `running_ == false` does not imply `server_thread_` is non-joinable. A simple join from two concurrent `stop()` callers is also unsafe.

## Decision

`join_server_thread()` moves the thread under `server_thread_mutex_`, so exactly one caller owns the join. The join happens outside the mutex; other callers wait for its completion before returning to code that may destroy the server. A generation check keeps a waiter from adopting a newly started thread. The completion notification occurs while the mutex is held, before the last access to the condition variable. `stop()`, `wait()`, `start_async()`, and teardown use this ownership protocol. Ownership is released on every exit path: a scope guard clears the in-flight flag and notifies whether the join returns or throws. A `join()` that fails does not get a second attempt: `libstdc++` only clears a thread's id on a *successful* join, so a retry from `join_orphaned_threads()` (which runs under `lifecycle_mutex_`) is not provably non-blocking, and could deadlock against this same thread's own spawn phase waiting on that mutex. `join_or_abandon()` instead falls back to `detach()`, and — if `detach()` also throws, which should not be reachable in practice — parks the `std::thread` object in `abandoned_threads()`, a leaky-singleton `std::vector<std::thread>` reached through a `static auto*` that is never deleted, rather than let a still-joinable thread destruct and call `std::terminate()`. Two requirements at once, not one: the container must stay reachable (so LeakSanitizer, the `sanitizers` CI job, does not report the parked thread as an indistinguishable ordinary leak — leaked by design, not by omission), and the parked `std::thread` objects must never be destroyed at all, since both `join()` and `detach()` having thrown leaves them still `joinable()`. A plain `static std::vector<std::thread>` (not heap-allocated) satisfies only the first: its non-trivial destructor runs via `__cxa_atexit` at normal process exit and would call `std::terminate()` there instead of never — the same relocated-abort failure mode this decision exists to close. A never-deleted heap allocation reached through a static pointer satisfies both. `join_orphaned_threads()`, and every other raw lifecycle `.join()` in `stop()` (`rtc_probe_thread_`, `reactor_thread_`, each worker), use the same helper, for the identical reason — `stop()` is reached from `~Server()` and from a detached shutdown-callback thread (`handle_shutdown_request()`), both contexts where an escaping `system_error` is at minimum a contract break and at worst another `std::terminate()`.

`join_or_abandon()` never rethrows: `stop()` and `wait()` both return normally even when the underlying thread had to be detached rather than confirmed joined, with only a logged `ERROR` distinguishing the two. This was a deliberate call, not an oversight: propagating the failure only from `wait()` (leaving `stop()` quiet, since `stop()` must not throw — that is the #402 lesson, a port conflict becoming an abort at a different layer) would need a second parameter threaded through `join_server_thread()`/`join_or_abandon()` and a new exception type, for a fault path (`ESRCH`/`EINVAL` on a handle that isn't the caller's own thread) neither this decision nor the review that revised it could demonstrate is reachable through the current test harness. Adding untested branching to the most safety-critical function in this file for an unfalsifiable path is the wrong trade; the log line is the intended signal until a concrete need for a stronger one appears.

## Alternatives rejected

Return early when `running_` is false: leaves a failed-start thread joinable. Join directly from each caller: concurrent joins are undefined behavior. Detach the worker as the NORMAL teardown path (not the `join_or_abandon()` fault-path fallback below), or let a losing caller return while another joins: teardown can free state the worker still uses. Hold the ownership mutex through `join()`: can deadlock while the accept loop unwinds.

## Consequences

A failed bind remains a recoverable start failure. A stop caller returns only after the thread for its generation is gone. Any new path that reads, moves, assigns, or joins `server_thread_` must participate in the same protocol.

## Links

- Issue [#402](https://github.com/open-astro/AlpacaBridge/issues/402); PR [#428](https://github.com/open-astro/AlpacaBridge/pull/428).
- Issue [#507](https://github.com/open-astro/AlpacaBridge/issues/507); PR [#541](https://github.com/open-astro/AlpacaBridge/pull/541) (the `join_or_abandon()` fallback and the `wait()`-race test).
- Implementation: `AlpacaHTTP/src/http/server.cpp`, `AlpacaHTTP/include/alpacahttp/server.h`, `AlpacaHTTP/src/http/thread_join.h` (`join_or_abandon()` and `abandoned_threads()` moved here, issue #561).
- Regression tests: `AlpacaHTTP/tests/test_server_socket.cpp` port-conflict, same-object retry, concurrent-stop, stop()/wait() race, and `join_or_abandon()`-fallback (issue #561) cases; `AlpacaHTTP/tests/test_thread_join.cpp` for the fallback in isolation.
