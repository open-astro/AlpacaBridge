### Fixed
- **ToupTek / Altair Thermal Switch** (AlpacaCore): turning the camera fan off while the cooler runs no longer silently stops the cooler. On the ATR2600M the firmware stops the cooler with the fan and leaves it off when the fan returns, so a client that switched the fan off and on again lost its cooling with nothing reported. The `Fan` element now refuses 0 (off) while the camera reports its cooler on, and its description says so; see Breaking changes (issue #1993).

### Breaking changes
- **ToupTek / Altair Thermal Switch**: setting the `Fan` element to 0, or `SetSwitch` false on it, now fails with `InvalidOperation` ("Turn the cooler off (CoolerOn = false) before turning the fan off ...") while the same camera's cooler is on; other fan speeds still write. **After upgrading:** a client or sequence that turns the fan off must set the camera's `CoolerOn` to false first.
