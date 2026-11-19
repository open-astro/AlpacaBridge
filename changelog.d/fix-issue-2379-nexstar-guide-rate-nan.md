### Fixed

- **Celestron NexStar: the per-axis guide-rate setters refuse a NaN or infinite rate with `InvalidValue` while disconnected** (`celestron_telescope_driver.cpp`), issue #2379. `GuideRateRightAscension` and `GuideRateDeclination` used to read the guide rate first and answer `NotConnected`, while `set_guide_rate()` already answered `InvalidValue` for the same value. A negative rate while disconnected still answers `NotConnected` (#2257 decision).
