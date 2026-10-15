### Breaking changes

- **Router: routes outside the Alpaca schema are removed**, AlpacaHTTP and AlpacaCore, issue #576. `/api/v1/mount/...` (an alias of `telescope`), `dome/{n}/canslew` and `observingconditions/{n}/seeing` now answer HTTP 400 ("Unknown device type: mount", "Unknown method: ..."); the schema has no such device type or member (`StarFWHM` is the seeing property). The dome driver's `get_can_slew()` and the observing-conditions `get_seeing()` interface members are gone. Covered by the "routes outside the Alpaca schema are rejected" case in `AlpacaHTTP/tests/test_routing.cpp`.

### Fixed

- **Switch: `cancelasync` no longer reports a cancel that never happened**, AlpacaHTTP, issue #576. GET and PUT `/api/v1/switch/{n}/cancelasync` now return `NotImplemented` (0x400) whatever `CanAsync` is, because the switch driver has no cancel member; before, an async-capable switch got a plain success. Asserted on an async-capable switch in `AlpacaHTTP/tests/test_routing.cpp`.
