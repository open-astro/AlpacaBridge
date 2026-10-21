### Breaking changes
- **ZWO cameras: AbortExposure during image download now returns InvalidOperation**: clients that previously called AbortExposure after ImageReady became true must stop doing so. **After upgrading:** call AbortExposure only while exposure is active; the completed image is preserved during readout.

### Fixed
- **ZWO camera: keep StopExposure distinct from AbortExposure during image download** (AlpacaCore, related to PR #1111): StopExposure no longer disrupts an accepted image readout; AbortExposure reports InvalidOperation when a lazy image download is already in progress. ImageReady stays false while a new exposure is starting. For the AbortExposure behavior change, see Breaking changes.
