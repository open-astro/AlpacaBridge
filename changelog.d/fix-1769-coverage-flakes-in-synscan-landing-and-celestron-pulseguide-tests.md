### Fixed

- **SynScan landing and Celestron PulseGuide test flakes**, tests only, issues #1769 and #1770. The positive waits in `test_synscan_goto_landing.cpp` and `test_celestron_async_slew_failure.cpp` now end on 60 s of real time instead of a virtual deadline or step count, which another thread's parked waiter could use up in milliseconds; the SynScan superseded-landing case joins its slew thread on every exit, so a failed assertion no longer aborts the run; the two Celestron pulse-chain cases wait for their chains to park before the clock moves.
