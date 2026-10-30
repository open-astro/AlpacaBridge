### Fixed

- **ASIAIR Plus connect no longer drives PWM ports on**, ZWO ASIAIR Plus switch driver, issue #1904. Connecting wrote SET_MODE, ENABLE and SET_LEVEL (ON) to every PWM-enabled port because the worker started at the cached value 100. A PWM port's worker now writes nothing until a client write or the server-start power-on pass sets its value. Covered by `AlpacaCore/tests/test_zwo_asiair_plus_switch.cpp` ("connect writes nothing to a PWM port no client set"); [decision 0021](docs/decisions/0021-power-port-power-on-state.md) rule 1.
