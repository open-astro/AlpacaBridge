### Fixed

- **The telescope `UTCDate` host-clock contract is now stated and pinned** (AlpacaHTTP, issue #1808): the host-clock step from a client's `UTCDate` PUT runs before the driver write and does not depend on the driver accepting it; the driver's own error (`PropertyNotImplemented`, `NotConnected`) still goes back unchanged. New router tests in `test_routing.cpp` cover a rejected write, a failed step and a malformed date; no behaviour change.
