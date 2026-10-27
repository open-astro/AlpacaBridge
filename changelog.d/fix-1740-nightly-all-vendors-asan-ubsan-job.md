### Changed

- **Nightly all-vendors ASan + UBSan job**, CI, issue #1740. The nightly workflow now builds every vendor with ASan and UBSan and runs the full suite, which no other CI job does for the vendor tests (`.github/workflows/nightly-build.yml`, job `test-sanitizers`). It does not gate publishing the nightly `.deb`.
