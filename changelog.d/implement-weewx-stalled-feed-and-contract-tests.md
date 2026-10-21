### Changed

- **WeeWX stalled-feed and contract tests**, WeeWX driver, issue #1009. `FakeWeeWxFeed` gains stall, drop and HTTP 500 modes, and `AlpacaCore/tests/test_weewx_observingconditions.cpp` now covers a feed that stops answering or returns garbage after connect (Connected holds, last values and their growing age are kept, polling recovers), Refresh, SensorDescription, every IObservingConditionsV2 member connected, and the value-range and state-machine contracts. Tests only.
