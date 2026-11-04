### Added
- **CaptureSession** (AlpacaCore): `CaptureSession::start` takes an optional per-start watchdog margin that replaces the constructor's margin for that exposure only, for a camera whose exposure body length depends on the exposure (retries, a bulb file wait). Existing callers are unchanged (issue #1979).
