# Failed HTTP bind left a joinable server thread

## Summary

A busy port produced a clean `is_running() == false` result, then `~Server()` aborted through `std::terminate()`.

## Root Cause

`start_async()` had already stored a `std::thread`. The worker's failed `bind()` set `running_ = false` and returned. `stop()` treated that flag as proof there was nothing to join, so teardown destroyed a joinable thread. A retry could also assign over it and abort. Concurrent stop calls exposed a second ownership race in a simple join fix.

## Prevention

Reap a joinable thread on every failure and retry path. Use the single-owner, waiter-aware join protocol recorded in `docs/decisions/0002-server-thread-ownership.md`; do not infer thread ownership from `running_`.

## Evidence

- Issue [#402](https://github.com/open-astro/AlpacaBridge/issues/402); PR [#428](https://github.com/open-astro/AlpacaBridge/pull/428).
- Regression tests: `AlpacaHTTP/tests/test_server_socket.cpp` port-conflict, same-object retry, and concurrent-stop cases. The port-conflict case fails if it cannot bind an ephemeral port (issue #507 moved it off a fixed literal), because a skip would otherwise look green.
