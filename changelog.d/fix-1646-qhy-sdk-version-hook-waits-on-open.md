### Fixed

- **QHY SDK version no longer waits on a connect**, QHY camera, issue #1646. `QHYSDKWrapper::get_sdk_version()` now reads its cache under a separate leaf mutex instead of the mutex `open_camera()` holds for the whole USB open, so `configureddevices` and `DriverInfo` return at once during a connect. Covered by "FakeQHYSDK via LockedQHYSDK - SDK version hook does not wait on an in-flight open" in `AlpacaCore/tests/test_qhy_camera.cpp`, which asserts the hook returns through the test decorator in under 100 ms while a 300 ms open is in flight.
