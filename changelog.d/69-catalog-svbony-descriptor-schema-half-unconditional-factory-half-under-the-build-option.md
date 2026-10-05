### Changed
- **SVBONY camera moved into the device catalog** (AlpacaCore, AlpacaHTTP): the schema (`svbony_schema.cpp`, every build) and factory (`svbony_catalog.cpp`, only under `ALPACACORE_ENABLE_SVBONY`) replace the router's register arm, its sanitize branch and its driver include. Behaviour is unchanged; `GET /management/v1/devicecatalog` now lists the `svbony` camera.
