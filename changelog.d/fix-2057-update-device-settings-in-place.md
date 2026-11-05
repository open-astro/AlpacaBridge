### Added

- **In-place telescope optics update**, AlpacaHTTP, issue #2057. `POST`/`PUT /management/v1/updatedevice` sets a registered telescope's `apertureDiameter` and `focalLength` (metres) on the live driver and in its persisted entry without removing the device, so the connection and `uniqueId` stay; unknown keys, non-telescope or unregistered devices and negative, non-finite or wrongly typed values are refused with HTTP 200 and a non-zero `ErrorNumber`, and a cross-origin request is refused with 403. Covered by the `updatedevice` block in `AlpacaHTTP/tests/test_routing.cpp`.
