### Fixed

- **QHY CCDTemperature connect test no longer flakes on slow builds**, QHY camera tests, issue #1307. The case `QHY Camera Driver - CCDTemperature is readable right after connect` now holds the fake's CURTEMP at the seeded -5.0 (`temp_settle_step_c = 1e-9`), so the temperature-control worker stepping toward 0 °C cannot move it before the read; it still fails when the connect-time seed is removed (`AlpacaCore/tests/test_qhy_camera.cpp`). No driver change.
