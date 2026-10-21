# iOptron iGuider 1 guide camera: protocol notes

Our own summary for the AlpacaBridge driver (`ioptron_iguider_camera_driver.cpp`,
`ioptron_iguider_v4l2_wrapper.cpp`). No vendor SDK or protocol document exists.
The decompiled vendor driver and its installer are not committed.

## Sources

1. **Vendor Windows driver**, `iOptron_iGuider_ASCOM_Installer_1320.exe` (Inno Setup 6.1.0):
   `ASCOM.iOptroniGuider.Camera.dll` 1.3.0.0 and `iOptron iGuider.exe`, both .NET and
   obfuscated with SmartAssembly (encrypted strings, proxied member references), decompiled
   with ilspycmd 9.1. Update history 1.3.2.0 (2024-10-25, Platform 7). "iOptron iGuider 1"
   support was added in 1.1.2.0 (2020-12-02).
2. **INDI** `indi_v4l2_ccd` (`indilib/indi`, `drivers/video/v4l2driver.cpp`) and its iOptron
   special cases, in particular PR #2012 (2024-03-08), whose author sniffed the Windows
   driver on the USB bus, and issue #1722.
3. **Our probe** of a unit on 2026-10-20 (VMware arm64 VM, kernel 6.12, `v4l2-ctl`). Frames
   could not be captured there; see "Confirmed on the Pi rig (2026-10-21)" for the frames.

## Device

- USB `0130:1466`, manufacturer and product strings "iOptron iGuider External", USB 2.0
  high speed, `bcdDevice` 1.00. A standard UVC device; Linux binds `uvcvideo`, which makes
  a capture node and a metadata node (`/dev/videoN`, `/dev/videoN+1`).
- V4L2 card name `iOptron iGuider External: iOptr` (INDI also lists `iOptron iGuider: iOptron
  iGuide` and `iOptron iGuider 1` for other units or firmware). The driver matches "iguider"
  in the card name, case-insensitively.
- Streaming is isochronous (alt settings 1-6, up to 3x1024 bytes). VMware's virtual EHCI
  controller delivered no frames at all; use real hardware or a USB 3 virtual controller.
- UVC extension unit GUID `{28f03370-6311-4a2e-ba2c-6890eb334016}`, 24 controls. The
  Windows app never uses it (no `IKsControl`/`IKsPropertySet` calls), so neither do we.

## Architecture of the Windows driver

`iOptron iGuider.exe` owns the camera through DirectShow (`quartz.dll`, Running Object
Table, `ISampleGrabber`). It writes each frame as 1280x960 8-bit mono (1,228,800 bytes) into
a memory-mapped file and takes settings from the registry. The ASCOM DLL is a thin client:
`Connected` waits up to 60 s for the EXE's process, reads the mapped frame, and writes the
exposure index and gain for the EXE to apply. The EXE starts streaming when it starts and
discards frames nobody asked for.

## Formats

| Format | Size | Rate |
| --- | --- | --- |
| YUYV | 640x960 | 18 fps |
| YUYV | 1280x960 | 9 fps |

The driver uses 1280x960 YUYV and keeps the Y bytes (every even byte): a 1280x960 8-bit
mono frame, as INDI does (issue #1722 log: a 2,457,600-byte buffer decoded to 1,228,800).
640x960 YUYV is also exactly 1,228,800 bytes; what it carries was not checked.

## Controls

| V4L2 control | Range | Driver use |
| --- | --- | --- |
| `auto_exposure` (0x009a0901) | 1 manual, 3 aperture priority (default) | set to 1 at connect |
| `exposure_time_absolute` (0x009a0902) | 1..5000 | an exposure **step code**, below |
| `gain` (0x00980913) | 64..480, step 32, default 64 | Gain g = 0..13 maps to 64 + 32g |
| brightness, contrast, saturation, hue, gamma, sharpness, backlight, white balance, power line | | left at defaults |

The Windows app's gain dropdown sends index 0 as 64, index 1 as 96, and index n as 64n; the
vendor ASCOM driver fixes Gain at 1 (GainMin = GainMax = 1). We expose the full control.

### Exposure steps

The firmware does not read `exposure_time_absolute` as 100 us units. The Windows app sends
`index - 13` through DirectShow's log2-seconds scale, and Windows turns that into these
values on the wire (USB sniff, INDI PR #2012):

| Duration (s) | 0.01 | 0.02 | 0.05 | 0.1 | 0.2 | 0.5 | 1.0 | 1.5 | 2.0 | 2.5 | 3.0 | 3.5 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Value | 1 | 2 | 5 | 10 | 20 | 39 | 78 | 156 | 312 | 625 | 1250 | 2500 |

Linear to 0.2 s, then `19.53 * 4^duration` (5000 would be 4 s). The vendor ASCOM driver
rounds `StartExposure(Duration)` to these steps with thresholds 0.015, 0.035, 0.075, 0.15,
0.35, 0.75, 1.25, 1.75, 2.25, 2.75 and 3.25 s, and reports ExposureMin 0.01, ExposureMax 3.5,
ExposureResolution 0.01. Longer exposures are not offered (INDI stacks frames instead).

## Sensor figures (vendor ASCOM driver constants)

1280x960, 3.75 um pixels, monochrome, MaxADU 255, 1.28 e-/ADU, full well 14,500 e-. No
binning, cooling, shutter, abort or stop, and no ST-4 port (CanPulseGuide false); guiding
goes through the mount. The sensor name string is encrypted in the DLL; the figures suggest
an Aptina AR0130, unconfirmed, so SensorName is reported empty.

## Quirks the driver works around

1. **Do not stop the stream between exposures.** After a stream is stopped, the iGuider
   often fails to restart on Linux: `Resource busy` on a format change, a stuck 9 or 20 fps
   rate, or exposure writes ignored. Windows issues `ABORT_PIPE` then
   `SYNC_RESET_PIPE_AND_CLEAR_STALL` when it stops; uvcvideo only aborts (PR #2012). The
   Windows driver streams from start to finish, and so do INDI and our driver (stream from
   connect to disconnect, take the first suitable frame for each exposure).
2. **Start at the shortest exposure.** A stream started at a long exposure delivers its first
   frame very late (1 s: 4 s, 2 s: 8 s, 3 s: 12 s, 4 s: 27 s). Starting at value 1 and
   setting the requested value only after the first frame gives a first frame in about
   150 ms (PR #2012). Our connect waits up to 3 s for that frame.
3. **Stalls.** The Windows driver restarts the stream when no frame arrives for 10 s. Our
   driver latches a link fault after 10 s without a frame (CameraState Error, StartExposure
   refused), restarts the stream, and clears the fault on the next frame. A fault that stands
   for 30 s, or an unplug (ENODEV), drops `Connected` until the client connects again
   (decision 0009). INDI instead fails the exposure and stops, because its restart could loop.

## Confirmed on the Pi rig (2026-10-21)

On a Raspberry Pi CM4 (real USB, kernel uvcvideo), with `v4l2-ctl`:

- **Frames stream** at 9.5 fps in 1280x960 YUYV, every buffer complete (2,457,600 bytes).
  VMware's virtual EHCI controller never delivered a frame, before or after a VM power cycle
  with USB 3.x compatibility selected (the camera still landed on EHCI); test on real hardware.
- **The Y plane is the sensor image**: every odd (chroma) byte is 128 with no variation, and
  neighbouring luma pixels differ equally across and down (0.72 vs 0.73), as a true 1280x960
  frame does.
- **The tick table is a step code**: with the stream running, tick 78 gives exactly 1.000 s
  between frames and tick 312 exactly 2.000 s; the frame period follows the exposure.
- **A control write lands 2 to 3 frames late**: the new exposure first showed on the 3rd frame
  after the write (tick 1 to 39) or the 4th (39 to 1, 1 to 78). The driver therefore uses the
  4th frame after any write (`kSettleFrames`), and an exposure change waits out up to three
  frames at the old exposure (3.5 s to 0.01 s costs about 10.5 s once).
- **Stopping and restarting the stream** worked every time (five times in a row) on this host;
  the restart failure INDI reports did not reproduce here, but the driver keeps the stream
  running anyway.
