# GPIO soft-PWM composition: adoption

The roster of [decision 0027](../decisions/0027-gpio-soft-pwm-composition.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `OwnedSoftPwmPorts`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| ZWO ASIAIR Pro and Plus CM4 switch | `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp` | adopted | #1757 | over `util::GpiodLineBackend` |
| ZWO ASIAIR Plus RK3568 switch | `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp` | adopted | #1757 | over `PwmGpioMiscBackend` |
| iOptron iMate PowerBox switch | `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp` | adopted | #2722 | over `util::GpiodLineBackend` |
| ToupTek StellaVita switch | `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp` | adopted | #2722 | over `util::GpiodLineBackend` |
