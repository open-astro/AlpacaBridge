### Changed (internal)

- **AlpacaHTTP router: the Camera method dispatch moves to its own source**, issue #2081. `Router::dispatch_camera_method` now lives in `AlpacaHTTP/src/http/routes/camera_routes.cpp`, byte-identical; `dispatch_device_method` stays in the router. No request, response or error text changes.
