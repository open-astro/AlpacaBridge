### Fixed

- **ZWO camera first CCDTemperature read after connect**, ZWO camera driver, issue #722. The first `CCDTemperature` read of a connection no longer returns a placeholder `0.0` that the SDK reports as success: while that first read is 0, the driver re-reads up to 5 times at 50 ms intervals and returns the first non-zero value, or `0.0` after the bound (a real 0.0 degrees C stays reportable). Later reads are unchanged, and a reconnect re-arms the handling. Covered by the two `ZWO fake SDK - ... CCDTemperature read ...` cases in `AlpacaCore/tests/test_zwo_fake_sdk.cpp`.
