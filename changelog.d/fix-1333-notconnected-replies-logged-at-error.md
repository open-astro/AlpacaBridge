### Fixed

- **A client polling a disconnected device no longer fills the log with ERROR lines** (AlpacaHTTP, issue #1333): the router logs a NotConnected (1031) reply at DEBUG, like NotImplemented, InvalidValue and an early ImageArray, through `is_expected_not_connected()` in `router.cpp`; the reply is unchanged and connect failures logged elsewhere stay ERROR; pinned by `test_router_log_level`, which reads a disconnected focuser's `position` and asserts ErrorNumber 1031 and a DEBUG line.
