### Fixed

- **Player One camera connect no longer blocks identity getters**, Player One camera driver, issue #1841. The connect handshake runs under `transition_mutex_` with `mutex_` released, so `Name`, `UniqueID`, `SensorName` and `Connected` answer while the SDK opens the camera (decision 0016).
