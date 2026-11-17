### Changed

- **iOptron iGuider: a second StartExposure or a geometry write while an exposure is in flight is refused**, iGuider camera driver, issue #2259. A StartExposure used to supersede the exposure in flight; it now answers `InvalidOperation`, as every other camera does, and BinX, NumX, NumY, StartX and StartY writes are refused the same way until the frame is in (`iOptron iGuider Camera Driver - A new exposure or a geometry write is refused while one is in flight`).

### Fixed

- **QHY cameras run a 0 s exposure at ExposureMin instead of 0 microseconds**, QHY camera driver, issue #2259. `StartExposure(0)` (a bias frame) and any duration below the camera's minimum now write the minimum to the SDK (`QHY Camera Driver - StartExposure below ExposureMin runs at the minimum`).
- **Camera arguments are refused in the same order on ToupTek, Altair, QHY and GPhoto**, issue #2259. A bad bin, NumX, NumY, StartX or StartY, and a duration too large to convert to microseconds, answer `InvalidValue` before the connection check; while an exposure runs a bad bin answers `InvalidValue` before the exposing refusal on ToupTek, Altair and QHY; and GPhoto refuses a NumX or NumY of 0. The contract sweep's exemption rows for these drivers are gone; GPhoto's while-exposing writes stay exempt until #1982 and QHY's superseding StartExposure until #1736. ConformU not re-run.
