### Fixed
- **ZWO AM: coherent Park and FindHome completion** (AlpacaCore, issue #1704): AtPark and AtHome no longer report completion while the existing five-second startup guard still reports Slewing; completion still requires accepted stopped telemetry, and Park errors remain visible immediately.
