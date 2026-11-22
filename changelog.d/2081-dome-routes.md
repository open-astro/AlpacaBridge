### Changed (internal)

- **AlpacaHTTP router: the Dome method dispatch moves to its own source**, issue #2081. `Router::dispatch_dome_method` now lives in `AlpacaHTTP/src/http/routes/dome_routes.cpp`, byte-identical; `dispatch_device_method` stays in the router. No request, response or error text changes.
