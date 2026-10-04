### Fixed

- **Site latitude/longitude range errors from the router no longer print trailing zeros** (AlpacaHTTP, issue #735): `siteLatitude 200 is out of range: must be between -90 and 90 degrees` instead of `200.000000` / `-90.000000`, using the same number formatter as the catalog range messages.
