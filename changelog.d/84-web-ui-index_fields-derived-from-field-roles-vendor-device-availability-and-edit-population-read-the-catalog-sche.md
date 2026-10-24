### Changed

- **Web UI device form names its catalog fields** (AlpacaHTTP, issue #1618): every vendor and device-type block in `index.html` carries `data-vendor` / `data-device-type`, and every named field carries `data-config-key`; no field is renamed and no behaviour changes. `catalog_form_drift.test.js` (six cases) fails when the form and the device catalog disagree on a field.
