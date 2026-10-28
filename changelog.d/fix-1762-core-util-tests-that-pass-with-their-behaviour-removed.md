### Changed

- **Core util tests that pass with their behaviour removed**, AlpacaCore tests, issue #1762 (slice). The AsyncConnectable stress case now checks that a connect queued behind a running disconnect was run by the task tail; the firmware/SDK hook case uses a driver that overrides only the firmware hook; the HostClock default-construction and `ClientUtcWarning` null-probe cases compare against the kernel probe and its opposite; the `path_exists` case now covers an `EACCES` parent directory. Test-only.
