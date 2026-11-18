### Changed (internal)

- **AlpacaHTTP router: the SafetyMonitor method dispatch moves to its own source**, issue #2081. `Router::dispatch_safetymonitor_method` now lives in `AlpacaHTTP/src/http/routes/safetymonitor_routes.cpp`, unchanged, and is the first of one routes source per device type; `dispatch_device_method` stays in the router. Docs-drift check 14 (every `std::regex` is static) now reads the router and every `routes/*.cpp` source, with its "no regex found" floor over the union. No request, response or error text changes.
