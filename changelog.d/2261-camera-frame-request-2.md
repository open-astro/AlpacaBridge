### Changed (internal)

- **Camera frame request module: ToupTek/Altair, QHY, GPhoto and the iGuider adopt it**, issue #2261. Every camera driver now runs its StartExposure and geometry value checks through `util/camera_frame_request.h`, with its own message texts. QHY and GPhoto keep their ExposureMax checks in seconds and QHY its per-bin SDK query; the iGuider keeps its subframe value checks, whose text names the value. No error code changes.
