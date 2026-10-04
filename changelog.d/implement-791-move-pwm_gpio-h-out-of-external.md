### Changed

- **Moved the ASIAIR Plus `pwm_gpio.h` ioctl header out of `external/`** (AlpacaCore, issue #791): the reverse-engineered header is first-party, so it now lives at `AlpacaCore/src/vendors/zwo/pwm_gpio.h` where CodeQL scans it. A pure rename, no behaviour change; the udev rule stays under `external/ZWO/asiair-plus/`.
- **Pinned the ASIAIR Plus `pwm_gpio.h` ioctl struct sizes** (AlpacaCore): the three structs now use `int32_t` members and `static_assert` their kernel sizes (8, 12 and 8 bytes), so a layout change fails the build. Ioctl numbers and layout do not change.
