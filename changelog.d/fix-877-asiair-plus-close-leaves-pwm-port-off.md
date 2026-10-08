### Fixed

- **ASIAIR Plus close leaves a partial-duty PWM port off** (ZWO ASIAIR Plus RK3568): `close()` now drives each PWM port to the steady level of its duty (on above 0, off at 0) after the workers stop, as the ASIAIR Pro already does, instead of leaving the line wherever the soft-PWM cycle stopped. Covered by the `close settles a partial-duty PWM port ON` case in `AlpacaCore/tests/test_zwo_asiair_plus_switch.cpp`.
