### Fixed (build)

- **Nightly optimized build compiles again** (AlpacaCore, issue #1595): GCC 13 at -O3 on arm64 reported a false `-Werror=stringop-overflow` in the filter wheel `DeviceState` vector growth; `FilterWheelDriver::get_device_state()` now reserves both entries up front (`AlpacaCore/include/alpacacore/filterwheel_driver.h`). No behaviour change.
