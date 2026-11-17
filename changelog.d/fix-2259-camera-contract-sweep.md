### Changed

- **ZWO cameras accept a 0 s exposure and run it at the shortest exposure the camera supports**, ZWO camera driver, issue #2259. `StartExposure(0)` (a bias frame) and any duration below the SDK minimum used to answer `InvalidValue`; they now run at `ExposureMin`, as the SVBONY, ToupTek, Player One and QHY cameras do (`ZWO Camera Driver - StartExposure below ExposureMin runs at the minimum`).

### Fixed

- **Camera arguments are refused in the same order on ZWO, Player One (and iCAM) and SVBONY**, issue #2259. A bad argument is `InvalidValue` before the connection check: a NaN, negative or too-large `StartExposure` duration on Player One and iCAM, and a bin, NumX or NumY of 0 or less or a negative StartX or StartY on ZWO, Player One and iCAM. While an exposure runs, a bad bin, size or start on ZWO is `InvalidValue`, not "Cannot change camera ROI during an exposure". A disconnected SVBONY bin write answers `NotConnected`, not "Bin value not supported" (`SVBONY Camera Driver - a disconnected bin write answers NotConnected`). The canonical order is in the ASCOM contract topic file.

### Added (tests)

- **The contract sweep probes every camera**, issue #2259. The sweep no longer exempts cameras. It probes `StartExposure`, `BinX`, `BinY`, `NumX`, `NumY`, `StartX` and `StartY` with bad arguments while disconnected, connected and while an exposure runs (the exposure started through each camera's tier-3 recipe). Once connected it also probes a bin above `MaxBin`, a duration above `ExposureMax` and a frame wider than the sensor, and while an exposure runs it checks that a good value is refused with `InvalidOperation`. ToupTek, Altair, QHY, GPhoto and iGuider still drift and are listed in `kProbeExemptions` and `kExposingRefusalExemptions` until #2259 PR 2 (GPhoto's while-exposing writes until #1982). ConformU not re-run.
