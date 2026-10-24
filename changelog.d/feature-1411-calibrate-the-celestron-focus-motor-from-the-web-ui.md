### Added

- **Calibrate the Celestron focus motor from the web UI** (Celestron, issue #1411): the focuser device card has a Calibrate button (with an end-to-end travel confirmation), a live status read from the `CalibrationStatus` action every 2 seconds while a run is in progress, an Abort button sent only while that status says `Calibrating`, and a "not calibrated" hint. Parsing and wording are in `AlpacaHTTP/web/format.js`, pinned by `AlpacaHTTP/tests/web/celestron_calibration.test.js`.
