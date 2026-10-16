### Breaking changes
- **Player One and iCAM cameras: `StopExposure` returns a not-implemented error** (AlpacaCore, issue #1023): `CanStopExposure` is now false, and `StopExposure` returns `MethodNotImplemented` instead of ending the exposure. 4.2.0 reported `CanStopExposure` true. **After upgrading:** call `AbortExposure` to end an exposure early; clients that read `CanStopExposure`, NINA among them, already do.

### Fixed
- **Player One and iCAM cameras: `StopExposure` locked the camera until the exposure deadline** (AlpacaCore, issue #1023): the 5.0 driver expected `POAStopExposure` to finish readout early and publish a partial frame. On hardware (Ceres 462M, SDK 3.10.0) it discards the frame: the camera is idle again within 60 ms, `POAImageReady` never turns true and `POAGetImageData` fails. A 120 s exposure stopped at 10 s therefore held `CameraState` at Exposing for 125 s and then failed. `AbortExposure` is unchanged and returns the camera to Idle within 0.15 s; for the `StopExposure` change, see Breaking changes.

### Changed (tests)
- **Player One fake SDK discards the frame on stop** (issue #1023): `FakePlayerOneSDK::stop_exposure` no longer marks the image ready, and a download after a stop fails as it does on hardware. The three cases that relied on a partial frame are replaced by two: `StopExposure` is refused without touching the SDK and the exposure still completes, and `AbortExposure` discards an exposure, keeps a finished frame and leaves the camera ready for the next one.
