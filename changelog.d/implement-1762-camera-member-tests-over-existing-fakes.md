### Added (tests)

- **Camera member tests over the existing fakes**, QHY, ToupTek and gphoto camera drivers, issue #1762. Connected cases in `AlpacaCore/tests/test_qhy_camera.cpp`, `test_touptek_camera.cpp` and `test_gphoto_camera.cpp` call the camera members that no test called before and assert the value the fake configures or the ASCOM error code, including the `InvalidValue` boundaries of ROI and binning and `NotConnected` while disconnected. `AlpacaCore/tests/fake_touptek_sdk.h` gains two optional knobs, `raw_format` and `pixel_size_um`. Tests only; no driver changes.
