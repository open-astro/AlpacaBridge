### Fixed

- **QHY CCDTemperature readable right after connect** (QHY camera, issue #941): connect now seeds the cached CCD temperature with one CURTEMP read before the telemetry worker starts, so a cooled camera no longer answers `InvalidOperation` until the first poll; a failed seed read still reports an error and never substitutes a value (`test_qhy_camera.cpp`, "CCDTemperature is readable right after connect").
