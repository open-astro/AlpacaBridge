### Fixed
- **QHY camera: AbortExposure and Disconnect no longer wait out a running exposure** (AlpacaCore, issue #2721): the exposure body now polls `GetQHYCCDExposureRemaining` on the task clock, never past the requested exposure, and calls `GetQHYCCDSingleFrame` (which has no timeout) only once the exposure is over, so a stop during the exposure returns within one 100 ms poll step even when `CancelQHYCCDExposingAndReadout` does not interrupt the SDK. A stop during the readout of an ended exposure still waits for that readout. Covered by two fake-clock cases in `AlpacaCore/tests/test_qhy_camera.cpp`; decisions 0020 and 0006 QHY rows updated.

### Changed (docs)

- **The ZWO camera meets decision 0020 rule 5** (docs, issue #2721): a rig run on SDK 1.41 showed that `ASIGetDataAfterExp` copies a frame the SDK has already transferred (8 to 10 ms for an 18 MB ASI533MC Pro frame, 2 ms for an ASI120MM Mini frame), not a multi-second USB download, so the 0020 ZWO camera row is `adopted` and two code comments no longer say otherwise.
