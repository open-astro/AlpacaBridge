### Changed

- **QHY integrated filter wheel locking**, QHY driver, issue #1395. `Name`, `Names`, `FocusOffsets` and `Connected` no longer wait behind a `Position` status read, a move or a connect: SDK calls on the shared handle run under a per-driver I/O lock and the state lock is never held across them. A `Position` or move that races a disconnect now reports `NotConnected` instead of a slot-count error. Covered by the "Getters do not wait behind" cases in `AlpacaCore/tests/test_qhy_filterwheel.cpp`.
