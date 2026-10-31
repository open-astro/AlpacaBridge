### Fixed

- **ToupTek camera CoolerOn and CoolerPower without a cooler**, ToupTek driver, issue #1914. On a camera without a cooler, `CoolerOn` (get and set, both values) and `CoolerPower` now raise `PropertyNotImplemented` (0x400) instead of answering `false`, returning quietly or answering `0.0`, and read or write no TEC register. A camera with a cooler is unchanged. Covered by "ToupTek Camera Driver - A camera without a cooler raises NotImplemented for CoolerOn and CoolerPower" in `AlpacaCore/tests/test_touptek_camera.cpp`.
