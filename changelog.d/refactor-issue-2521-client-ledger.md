### Changed

- **Client-connection ledger owns the per-client Connected bookkeeping** (AlpacaHTTP, issue #2521): which clients hold each device, when each was last seen, the 256-client cap, the 10-minute expiry and the per-device connect/disconnect mutex move out of `router.cpp` into `ClientConnectionLedger` (`AlpacaHTTP/src/http/client_connection_ledger.h`), still keyed by the driver's address. It also builds the address-qualified client key. Only `purge()` erases a device's op mutex; `clear()`, which runs while that mutex is held, cannot. The ledger takes its clock at construction, so `Router::set_client_clock_for_test` is gone and a router test passes the clock to the `Router` constructor. No behavior change.

### Added (tests)

- **Client-connection ledger cases** (issue #2521): `AlpacaHTTP/tests/test_client_connection_ledger.cpp` drives the ledger over a test clock with no Router or devices: last client out, anonymous clients, the per-device cap, expiry of a vanished client, a removed device, a clear while the op mutex is held, and what `configureddevices` reports.
