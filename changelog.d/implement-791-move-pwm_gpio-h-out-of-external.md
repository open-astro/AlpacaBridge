### Changed

- **Moved the ASIAIR Plus `pwm_gpio.h` ioctl header out of `external/`** (AlpacaCore, issue #791): the reverse-engineered header is first-party, so it now lives at `AlpacaCore/src/vendors/zwo/pwm_gpio.h` where CodeQL scans it. A pure rename, no behaviour change; the udev rule stays under `external/ZWO/asiair-plus/`.
