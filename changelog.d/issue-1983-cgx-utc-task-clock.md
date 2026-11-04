### Changed
- **Celestron CGX: UTCDate clock-step check and host-discipline re-check read the task clock** (AlpacaCore, issue #1983): the elapsed time since a UTCDate write and the 30 s re-check of host NTP discipline now read the driver's injected task clock instead of `steady_clock`, so tests can drive them; the UTC value stays on `system_clock`, and production behavior does not change.
