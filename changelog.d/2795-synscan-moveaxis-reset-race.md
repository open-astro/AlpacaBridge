### Fixed (tests)

- **SynScan MoveAxis reset-connection flake** (AlpacaCore tests, #2795). "SynScan MoveAxis - a dispatch on a reset connection loses the link at once" expected DriverException from a MoveAxis sent on a dropped connection, but the driver's 1 s status poll could meet the reset first, latch the lost link, and make MoveAxis refuse with NotConnected before it dispatched. The case now pauses the status poll, so the dispatch is what meets the reset. Test-only: no driver change.
