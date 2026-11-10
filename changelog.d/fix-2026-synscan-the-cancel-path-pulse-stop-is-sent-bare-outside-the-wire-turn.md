### Added (tests)

- **SynScan cancelled pulse stop order is pinned** (SynScan, issue #2026): the "a cancelled pulse stop precedes the replacement pulse's turn" case in `test_synscan_telescope.cpp` asserts the first pulse's stop is sent before the replacement pulse's rate command; the call-site comment in `run_pulse_stop_body` states why the stop needs no wire turn. Test and comment only, no behaviour change.
