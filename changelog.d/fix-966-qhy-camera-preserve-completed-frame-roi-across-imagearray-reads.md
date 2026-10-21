### Fixed
- **QHY camera `ImageArray` keeps the completed exposure's size** (AlpacaCore, issue #966): changing `NumX` or `NumY` after `ImageReady` turns true no longer reshapes, crops or zero-pads the finished frame; the driver records the ROI size when the frame completes. Covered by the two `ImageArray keeps the completed frame's NumX/NumY` cases in `test_qhy_camera.cpp`.
