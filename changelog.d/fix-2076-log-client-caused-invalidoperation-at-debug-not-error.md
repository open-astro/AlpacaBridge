### Fixed

- **Parked and slaved refusals no longer log at ERROR**, AlpacaHTTP, issue #2076. A driver reply of `InvalidWhileParked` (0x408) or `InvalidWhileSlaved` (0x409) is now logged at DEBUG like the other client-caused errors; a plain `InvalidOperation` (0x40B) stays at ERROR. Covered by `AlpacaHTTP/tests/test_router_log_level.cpp`.
