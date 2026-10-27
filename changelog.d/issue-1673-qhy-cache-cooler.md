### Breaking changes
- **QHY camera: failed configuration no longer leaves trusted control readings** (AlpacaCore, issue #1673): Gain/Offset reads fail after a failed write, and their limits and MaxBinX/Y also fail after an incomplete mode change. **After upgrading:** retry the failed configuration or reconnect before reading affected controls.
- **QHY camera and integrated wheel opens wait for cooler OFF cleanup** (AlpacaCore, issue #1726): a new connection is refused while any OFF operation is in flight, including a normal OFF with the camera still connected. An OFF queued behind a download also delays integrated-wheel connections until cleanup finishes. **After upgrading:** retry after cleanup finishes; a permanently wedged vendor call requires restarting the server.

### Fixed
- **QHY camera control getters do not wait behind downloads** (AlpacaCore, issue #1673): cache Gain, Offset, their limits and MaxBinX/Y at connect and refresh after writes and mode changes, keeping actual HDR readback separate from requested restoration values; for failed configuration, see Breaking changes.
- **Explicit QHY cooler OFF survives disconnect** (AlpacaCore, issues #1673 and #1726): retain the original physical handle until one PWM-zero attempt after regulation stops, without blocking logical disconnect indefinitely or changing cooling on plain disconnect. Every accepted OFF also stops current regulation, including OFF → ON → OFF while the first zero is in flight.
- **QHY cooler OFF survives cancelled-worker retirement** (AlpacaCore, issue #1726): coordinate a new OFF with an exiting superseded worker so zero is sent while still connected; clear an unprepared old request on successful reconnect so a later plain disconnect does not send an OFF from the previous session.

### Changed
- **QHY miniCam8M validation report uses the canonical path** (AlpacaCore, issue #1673): replace the existing Linux arm64 ConformU report with the passing control-cache run instead of keeping an extra copy; leave the separate USB-recovery reports unchanged.
