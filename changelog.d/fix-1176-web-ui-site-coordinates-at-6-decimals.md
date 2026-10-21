### Fixed

- **The device card's Configured Settings list shows site latitude and longitude at 6 decimals** (Web UI, issue #1176): `formatSiteCoordinate()` in `format.js` prints `-33.8567844444444` as `-33.856784` and `151.215297171661` as `151.215297`, and a value that rounds to zero prints `0.000000`; display only, the API, the stored config and the edit forms keep full precision. Pinned by the `formatSiteCoordinate` case in `AlpacaHTTP/tests/web/format.test.js`.
