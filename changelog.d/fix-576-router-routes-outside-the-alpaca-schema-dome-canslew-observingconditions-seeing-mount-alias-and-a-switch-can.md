### Breaking changes

- **Router: routes outside the Alpaca schema are removed, and switch `cancelasync` stops succeeding**, AlpacaHTTP and AlpacaCore, issue #576. `/api/v1/mount/...` (an alias of `telescope`), `dome/{n}/canslew` and `observingconditions/{n}/seeing` now answer HTTP 400 ("Unknown device type: mount", "Unknown method: ..."); the schema has no such device type or member (`StarFWHM` is the seeing property). Clients that used them must switch to `telescope` and drop the two calls. GET and PUT `switch/{n}/cancelasync` on an async-capable switch now return `NotImplemented` (0x400) where they returned success; a client that called it must stop treating it as a cancel. Covered by the "routes outside the Alpaca schema are rejected (#576)" case in `AlpacaHTTP/tests/test_routing.cpp`.

### Fixed

- **Switch: `cancelasync` no longer reports a cancel that never happened**, AlpacaHTTP, issue #576. The switch driver has no cancel member, so the old success cancelled nothing; both verbs now answer `NotImplemented` whatever `CanAsync` is, while an out-of-range `Id` still answers `InvalidValue` (asserted on an async-capable switch in `AlpacaHTTP/tests/test_routing.cpp`). The dome `get_can_slew()` and observing-conditions `get_seeing()` interface members are removed with their routes.
