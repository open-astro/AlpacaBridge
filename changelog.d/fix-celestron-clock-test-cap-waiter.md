### Fixed (tests)

- **Celestron clock-trust tests no longer race the MoveAxis cap** (AlpacaCore tests, issue #2788): nine waits in `test_celestron_clock_trust.cpp` that run while a MoveAxis is moving now wait for three task-clock waiters (the paused poll, the re-sync and the MoveAxis cap body that #2651 added) instead of two, so the next `advance()` can no longer land before the re-sync parks. Under load the old waits failed 32 of 720 runs; the new ones failed none.
