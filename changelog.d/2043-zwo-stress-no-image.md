### Fixed
- **ZWO camera: a StopExposure or Disconnect could mark the next StartExposure as stopped before it ran** (AlpacaCore, issue #2043): the stop recorded the newest StartExposure's token, minted before its session start, so a start that was waiting for the stop's join was then refused its trigger and its exposure ended with "the exposure ended without an image". The stop now marks the exposure it found as stopping and records its session token in one hold (`util::CaptureSession::begin_stop()`), and the body compares the token with its own `CaptureControl::token()`.

### Added (tests)
- **CaptureSession begin_stop** (issue #2043): one case pins that the mark names the body that owns the exposure, is 0 otherwise, and drops that body's early return.
