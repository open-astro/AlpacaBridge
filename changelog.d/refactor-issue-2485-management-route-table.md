### Changed

- **One management route table** (AlpacaHTTP, issue #2485): `Router::parse_route()` and `Router::handle_management()` read one table, `Router::management_routes()`, instead of two hand-written chains over the same names. Each row holds the path and its unversioned alias, the accepted verbs, whether the origin guard applies, the wrong-verb family (400 with text on an Alpaca-defined path, 405 with `Allow` on an AlpacaBridge-only one) and the handler. The table applies the guard and answers a wrong verb, so the twelve guard calls and the wrong-verb branches in the handlers are gone. No behavior change: every status, error number and message is the same.

### Added (tests)

- **Management route walk** (issue #2485): `AlpacaHTTP/tests/test_route_table.cpp` section G checks the table against a fixture row by row, fails a row that accepts a verb other than GET with no guard, and sends five verbs to both spellings of every route, with and without a foreign Origin.
