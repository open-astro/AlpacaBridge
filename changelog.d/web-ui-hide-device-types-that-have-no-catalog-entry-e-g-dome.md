### Changed

- **The device form lists only device types the catalog can configure** (web UI, issue #1642): Device Type no longer offers a type with no available catalog entry (Dome today); it returns when a driver adds one. Editing a saved device of a hidden type still shows its type. While the catalog is not loaded the select stays disabled. Covered by the `offeredDeviceTypes` cases in `AlpacaHTTP/tests/web/catalog_schema.test.js`. ConformU not affected.
