### Added

- **ZWO EAF, EFW and CAA firmware in the management listing**, ZWO drivers, issue #2077. `GET /management/v1/configureddevices` now shows `Firmware` for a connected EAF focuser, EFW filter wheel and CAA rotator. Each driver reads the unit's firmware once at connect and serves it from a cache that Disconnect clears; a failed or empty read still connects and shows no `Firmware`. Covered by the "Firmware" cases in `AlpacaCore/tests/test_zwo_focuser.cpp`, `test_zwo_filterwheel.cpp` and `test_zwo_rotator.cpp`. ASI cameras and AM mounts are not covered.
