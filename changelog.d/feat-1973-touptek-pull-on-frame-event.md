### Changed

- **ToupTek and Altair exposures wait for the camera's frame event, then pull the frame** (ToupTek camera driver, issue #1973; [decision 0020](docs/decisions/0020-capture-session.md) rule 4). The exposure no longer blocks inside the SDK's `WaitImageV4` with its own exposure + 10 s timeout: the pull-mode callback wakes a cancellable wait on the task clock, and the body makes one non-blocking `PullImageV4` call. What a client sees:
  - AbortExposure, StopExposure and Disconnect end the frame wait at once, with no frame read, even when the camera never raises the frame event (test `AbortExposure during the frame wait returns the body promptly with no pull`).
  - An exposure whose frame never arrives now fails at the 15 s watchdog with `did not complete before its deadline`, where the SDK wait used to give up at the exposure + 10 s with `The camera failed to deliver the exposure frame`.

  Covered by fake-SDK tests only; not yet run on a real camera, so the decision 0020 row stays `partial`.
