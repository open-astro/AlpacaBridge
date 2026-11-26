### Fixed

- **SynScan stepped-call test helper**, SynScan tests, issue #2685. `call_stepping()` now steps the fake task clock only while the stepped call is parked on it, so a call held in real time on a slow runner no longer ages the paused status poll's frame past the 30 s loss bound ("SynScan mount link lost" from a getter after a stepped AbortSlew). Covered by the case "a stepped call held in real time does not age the paused poll's frame (#2685)" in `AlpacaCore/tests/test_synscan_clock_trust.cpp`.
