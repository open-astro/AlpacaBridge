### Fixed

- **QHY CFW3 Position during a disconnect**, QHY CFW3 filter wheel driver, issue #1699. A Position read that raced a disconnect could answer `DriverException` "Filter wheel position unknown"; it now answers `NotConnected`, as the Position PUT already did. Covered by `AlpacaCore/tests/test_qhy_cfw3_filterwheel.cpp`.
