### Fixed

- **SynScan UTCDate readback on the task clock**, SynScan driver, issue #1409. The written UTC instant plus the time since the write is now one `util::UtcOffset` read with `clock_.now()` (decision 0014), so a `FakeTaskClock` moves the readback: `AlpacaCore/tests/test_synscan_telescope.cpp` ("UTCDate readback advances with the task clock") writes a UTCDate, advances the clock 90 s and reads back the written value plus 90 s. The stored value is now also cleared on disconnect, not only on connect.
