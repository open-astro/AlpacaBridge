### Fixed (tests)

- **Two test or pre-flight runs on one machine no longer share a fixed `/tmp` path** (AlpacaHTTP tests and pre-flight, issue #2581). The software-update channel trap and marker cases in `test_software_update.cpp` each make a fresh `mkdtemp` directory and remove it when the case ends, and the pre-flight's scan-build log is written inside its own `AlpacaHTTP/build-scan` tree instead of `/tmp/scan_build_out.log`.
