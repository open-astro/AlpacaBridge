### Changed

- **Celestron UTCDate readback now runs on the driver's task clock** (Celestron, issue #1385): the three anchor fields become one `util::UtcOffset` (new header, decision 0014), so the readback is testable on `FakeTaskClock` (`Celestron UTCDate - a written UTCDate reads back advanced by the task clock and is dropped on reconnect`). The value is now also dropped on disconnect, not only on connect; production readback is unchanged.
