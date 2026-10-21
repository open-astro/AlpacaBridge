### Fixed

- **A saved connection type in mixed case, e.g. `Network`, is now honoured instead of falling back to serial** (AlpacaHTTP, AlpacaCore catalog, issue #508): `connectionType` is folded to lower case once, when a device config is registered and when it is stored, so `Network`, `SERIAL` and `Auto` behave exactly as `network`, `serial` and `auto` from the configuration API and from a saved file, and `configureddevices` lists the lower-case value. An unrecognised value (for example `carrier-pigeon`) is still refused by the API and read as `serial` from a saved file.
