### Fixed

- **ZWO dew heater getters on a racing disconnect**, ZWO switch driver, issue #1756. A getter that passed the connection check just before a disconnect now answers `NotConnected` (0x407) instead of `NotImplemented` (0x400, "Dew heater not supported"). Covered by the dew heater case in `AlpacaCore/tests/test_zwo_concurrency_stress.cpp`, whose guard now accepts `NotConnected` only.
