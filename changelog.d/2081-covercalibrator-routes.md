### Changed (internal)

- **AlpacaHTTP router: the CoverCalibrator method dispatch moves to its own source**, issue #2081. `Router::dispatch_covercalibrator_method` now lives in `AlpacaHTTP/src/http/routes/covercalibrator_routes.cpp`, byte-identical; `dispatch_device_method` stays in the router. No request, response or error text changes.
