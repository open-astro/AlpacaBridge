### Fixed
- **Source install** (`install_alpaca_service.sh`): `install` and `update` now create the `gpio` group when the host lacks it and add the service user to `dialout` and `gpio`, as `build_and_run.sh` already did. On stock Debian the ASIAIR power-port nodes otherwise fell back to root (`GROUP="gpio"` with no such group) and serial mounts failed with `EACCES` unless the user was already in `dialout` (issue #1698). `ALPACA_ADD_DIALOUT=OFF` and `ALPACA_ADD_GPIO=OFF` skip it.
- **Source build** (`build_and_run.sh`): its setup stamp now also covers the Altair and Player One Phoenix libraries and the QHY firmware, so an updated copy of any of them is installed on the next run (issue #1698).

### Changed
- **Source build and install scripts**: the udev, firmware, vendor-library and group setup moved into `scripts/host_setup.sh`, which both scripts source, with a self-test (`scripts/test_host_setup.sh`) in CI (issue #1698).
