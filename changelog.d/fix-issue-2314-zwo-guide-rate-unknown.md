### Fixed

- **ZWO AM GuideRate no longer reports a guessed rate**, ZWO mount driver, issue #2314. When the connect cannot read the guide rate, GuideRateRightAscension and GuideRateDeclination now raise `DriverException` until a later connect reads it or a client write sets it, instead of reporting an assumed 1.00x sidereal that the setter itself refuses. Covered by `AlpacaCore/tests/test_zwo_telescope.cpp`.
