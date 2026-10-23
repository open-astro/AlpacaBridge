### Fixed

- **SynScan tick-hunt wait on the task clock**, SynScan driver, issue #1403. The readback-tick hunt's wait for a poll tick in flight (bounded by 1500 ms) now reads `TaskClock` for its deadline and its 2 ms polling sleep, so a fake-clock test drives the bound. Covered by the case "the tick hunt waits out a poll tick in flight on the task clock (#1403)" in `AlpacaCore/tests/test_synscan_goto_landing.cpp`; SynScan is `adopted` in [decision 0005](docs/decisions/0005-task-clock.md).
