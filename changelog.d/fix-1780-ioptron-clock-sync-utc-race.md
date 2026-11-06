### Fixed

- **iOptron background clock sync no longer overwrites a client UTCDate write or retries with stale time**, iOptron driver, issue #1780. A client `UTCDate` write that lands after the background `:SUT` send now keeps the cached UTC anchor, so `UTCDate` reads back the client's value in wire order (a write generation, checked when the sync publishes). Each `:SUT` attempt reads the host clock when it sends, so a retry after the timezone and DST commands no longer sends the first attempt's time. Covered by the two cases in `AlpacaCore/tests/test_ioptron_clock_sync_slot.cpp`.
