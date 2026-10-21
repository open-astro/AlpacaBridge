### Fixed (tests)

- **SynScan park tests no longer drift with the wall clock**, SynScan, issue #976. The equatorial park cases in `test_synscan_async_park.cpp` pin the sidereal wall time through the new `set_sidereal_wall_clock_for_testing()` seam, so a real second passing between `set_park()` and `park()` on a slow build no longer moves the park GOTO by 0xC300 counts. The driver reads `std::chrono::system_clock` unless a test replaces it; hardware behaviour is unchanged.
