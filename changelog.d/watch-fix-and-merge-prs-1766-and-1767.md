### Changed

- **Docs and comment drift**, AlpacaHTTP and ZWO ASIAIR Plus, issue #1889. `AlpacaCore/PowerPorts.md` names both udev rule paths, `AlpacaHTTP/README.md` names four of the `ALPACAHTTP_*` environment variables that `Config::apply_environment_overrides` in `AlpacaHTTP/src/core/config.cpp` reads (`ALPACAHTTP_PORT`, `ALPACAHTTP_DISCOVERY`, `ALPACAHTTP_DISCOVERY_PORT`, `ALPACAHTTP_LOG_LEVEL`) and points to that function for the rest, and the ASIAIR Plus header comment states the 50 Hz default.
