### Added

- **ZWO camera form picks the attached camera** (web UI, AlpacaHTTP, issue #1970): the Camera Index box is replaced by a list of the attached ZWO cameras with their index, model and serial, read from the new `GET /management/v1/attachedcameras?vendor=zwo` (the same enumeration registration runs). A camera another ZWO camera entry holds by serial is greyed out, nothing is preselected, and the form will not save until a camera is picked. The entry saves the picked camera's `serialNumber` and `cameraName`, so it binds by serial even when other cameras are unplugged later. "Enter the index by hand" keeps the index and ID boxes for a camera that is not attached yet, and no longer auto-numbers the index from the count of saved ZWO cameras. Editing an entry that never learned a serial shows that, and picking its camera rebinds it.

### Fixed

- **ZWO camera registration says why an entry learned no identity** (AlpacaCore, AlpacaHTTP, issue #1969): when the index or ID hint matches no attached camera and the entry has no serial or model name, registration logs the hint, the reason and every enumerated camera (slot, model, serial), at WARN for a stored entry and INFO for a `configuredevice` call. The same text appears as `RegistrationWarning` on the device card (`configureddevices`) and after "Warning: " in the `configuredevice` reply, which still succeeds. The entry still registers, so an unplugged camera keeps its device.

### Added (tests)

- **Camera picker and unbound-entry warning cases** (issues #1969, #1970): `test_zwo_camera_catalog.cpp` over `FakeZWOSDK` (a saved serial binds its camera whatever `cameraIndex` says; the warning for an unmatched hint and for a failed enumeration; the attached-camera lister), the warning text in `test_zwo_camera_identity.cpp`, `attachedcameras` and `RegistrationWarning` routing cases in `test_routing.cpp`, and `node --test` cases for the picker model in `AlpacaHTTP/tests/web/zwo-camera-picker.test.js`.
