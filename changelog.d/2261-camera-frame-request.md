### Changed (internal)

- **Camera frame request module: ZWO, SVBONY and Player One share one set of StartExposure and geometry checks**, issue #2261. `util/camera_frame_request.h` holds the duration, exposure-range, bin, NumX/NumY, StartX/StartY and frame-on-sensor checks in the #2259 order as free functions; each driver keeps its connection and exposing checks, its SDK alignment and its own message texts. The iOptron iCAM rides the Player One driver. No error code, order or message changes; ToupTek/Altair, QHY, GPhoto and iGuider follow in the next pull request.
