### Fixed
- **Celestron and SynScan telescope motion ownership** (AlpacaCore, issue #769): aborts and replacement motions fence stale tasks; pulse-guide chains are independent per axis, and `MoveAxis` only cancels a pulse on the same axis.
- **Sky-Watcher synchronous slew cancellation**: a `SlewToCoordinates` superseded during its wait now reports `InvalidOperation` instead of `DriverException` or returning as if the slew completed.
- `AbortSlew` signals cancellation and sends the hardware stop before joining the async slew worker. A GOTO already in flight still must complete or reach the wrapper's configured response timeout before the serialized stop command can be sent.
