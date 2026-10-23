### Fixed

- **405 responses carry an `Allow` header**, AlpacaHTTP, issue #1492. Every 405 from an AlpacaBridge-only management path now lists the methods its handler accepts (RFC 9110 s15.5.6): `GET` for `faileddevices`, `devicecatalog` and `thermalcameras`; `POST, PUT` for `configuredevice`, `removedevice`, `shutdown` and `restart`; `GET, POST, PUT` for `synctime`. Status, body and error code are unchanged. Covered by `AlpacaHTTP/tests/test_routing.cpp`.
