### Changed

- **Management call module for the management endpoints** (AlpacaHTTP, issue #2486): `ManagementCall` (`AlpacaHTTP/src/http/management_call.h`) does for the `/management/...` handlers what `DeviceCall` does for the Device API. It reads `ClientTransactionID` once (the query string's, the name matched in any case; a non-zero JSON body ID replaces it once a handler reads the body), looks up a body field by its accepted spellings, builds the Alpaca JSON envelope with both transaction ids, and maps a caught exception to its error number. The log endpoints (`loglevel`, `logs`, `logfiles`, `logfiles/{name}`) are its first users and now keep only their own work; the other management handlers move to it in later PRs. No response a client sees changes.

### Added (tests)

- **Management call cases** (issue #2486): `AlpacaHTTP/tests/test_management_call.cpp` drives `ManagementCall` on a parsed request with no Router: the transaction ID from the query in any case and absent or unparsable, a JSON body ID replacing it and a form body never doing so, the body lookup by spelling, the success and error envelopes, and the exception map.
