# Camera capture session: adoption

The roster of [decision 0020](../decisions/0020-capture-session.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

The roster of camera drivers whose exposure runs in a session. `scripts/check_docs_drift.py` check 23 holds it to the code.

Scan: `AlpacaCore/src/vendors/*/*camera_driver.cpp`
Symbol: `CaptureSession`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Player One camera | `AlpacaCore/src/vendors/playerone/playerone_camera_driver.cpp` | adopted | #1605 | pilot; no `detach()` left |
| ZWO camera | `AlpacaCore/src/vendors/zwo/zwo_camera_driver.cpp` | adopted | #1981, #2721 | the exposure runs in a session; rule 5: `ASIGetDataAfterExp` takes no timeout but copies a frame the SDK already holds (rig, SDK 1.41: 8 to 10 ms for an 18 MB ASI533MC Pro frame on USB 3, 2 ms for an ASI120MM Mini frame on USB 2, #2721), so it is the one `get_image_data` call the rule allows |
| ToupTek camera | `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp` | partial | #2130 | the exposure runs in a session; rule 4: the body waits in `CaptureControl::wait_for_signal` until the SDK frame event, then makes one `Toupcam_PullImageV4` call; adopted after a run on a real camera is reported; Altair rides this driver |
| SVBONY camera | `AlpacaCore/src/vendors/svbony/svbony_camera_driver.cpp` | adopted | #1980 | rule 4: each bounded 500 ms `SVBGetVideoData` read is the poll step, and every pass goes through `CaptureControl::wait_for`, so the watchdog deadline ends the loop without a getter (ToupTek waits for its frame event instead) |
| gphoto camera | `AlpacaCore/src/vendors/gphoto/gphoto_camera_driver.cpp` | partial | #1982 | the exposure runs in a session and adds the actual exposure length (rule 7); rule 4: the native capture parks in `gp_camera_capture` (no cancel) and the bulb pumps wait inside bounded SDK calls, not `CaptureControl::wait_for` |
| QHY camera | `AlpacaCore/src/vendors/qhy/qhy_camera_driver.cpp` | partial | #2721 | the exposure runs in a session; no `detach()` left; rule 5: the body polls `GetQHYCCDExposureRemaining` through `CaptureControl::wait_for` and calls `GetQHYCCDSingleFrame` only once the exposure is over, so an abort or a disconnect during the exposure waits one poll step; `GetQHYCCDSingleFrame` has no timeout, so in the readout, when `cancel_exposure` does not interrupt it, they wait for that readout (unmeasured); adopted after a ConformU run on a real QHY camera is reported |
| iOptron iGuider camera | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | planned | #2720 | streaming UVC camera: an exposure is the first settled frame of the stream worker's `AsyncOperation` slot ([decision 0006](0006-async-operation-ownership.md)), not a session body; the deadline is on the task clock but not through `CaptureControl` (rules 1 and 4) |
