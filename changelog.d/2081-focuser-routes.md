### Changed (internal)

- **AlpacaHTTP router: the Focuser method dispatch moves to its own source**, issue #2081. `Router::dispatch_focuser_method` now lives in `AlpacaHTTP/src/http/routes/focuser_routes.cpp`, byte-identical; `dispatch_device_method` stays in the router. No request, response or error text changes.
