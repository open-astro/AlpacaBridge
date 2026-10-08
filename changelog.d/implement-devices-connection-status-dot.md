### Added

- **Devices tab shows a connection status dot after each device name** (AlpacaHTTP web UI): `GET /management/v1/configureddevices` now carries a boolean `Connected` per loaded device, omitted if `get_connected()` throws; the dot is green (connected), yellow (loaded, not connected) or red (`LoadError`, `LastConnectError` or unknown), with the state in its `title` and `aria-label`, and refreshes every 5 s only while the Devices tab is visible. Covered by `test_routing` ("configureddevices Connected field") and `deviceStatus` in `AlpacaHTTP/tests/web/format.test.js`.
