### Fixed

- **QHY colour frames have red and blue in the right planes**, QHY camera driver, issue #2689. A 3-channel 8-bit frame from `GetQHYCCDSingleFrame` is BGR per pixel, as the QHY SDK API manual's "Image data structure" section states for RGB24; the driver copied it unswapped, so `ImageArray` plane 0 held blue. It now swaps to R, G, B like the ZWO, SVBONY and Player One drivers. Covered by `AlpacaCore/tests/test_qhy_camera.cpp`.
