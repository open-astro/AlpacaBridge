# AlpacaBridge Supported Drivers

<img src="docs/image/ab.png" alt="AlpacaBridge logo" width="420">

## Updated 2026-08-12
This document lists all hardware vendors and device types that are verified to work with AlpacaBridge.

## Contents

- [General Notes](#general-notes)
- [Camera Drivers](#camera-drivers)
- [CoverCalibrator Drivers](#covercalibrator-drivers)
- [Dome Drivers](#dome-drivers)
- [FilterWheel Drivers](#filterwheel-drivers)
- [Focuser Drivers](#focuser-drivers)
- [ObservingConditions Drivers](#observingconditions-drivers)
- [Rotator Drivers](#rotator-drivers)
- [SafetyMonitor Drivers](#safetymonitor-drivers)
- [Switch Drivers](#switch-drivers)
- [Telescope Drivers](#telescope-drivers)

## General Notes

- **ConformU Verification**: All drivers listed below have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Development Guide](docs/development.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **Ethernet**: Network-based connection (TCP/IP)
  - **USB/Serial**: USB-to-serial adapter or direct serial connection

- **Linux Notes**:
  - **Debian 13 (Trixie) on arm64**: AlpacaBridge is built and validated on arm64 only (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate). All drivers have been tested using Debian 13 on arm64 with ConformU v4.2.1 (original drivers), v4.3.0, or v4.4.0 (newer drivers). As new ConformU versions are released this will be adjusted.
  - **Kernel 6.12.75-v8-16+ or higher.**: Note: kernel 6.12.75-v8-16+ is required to ensure ZWO EAF/EFW hardware compatibility. Without it, devices besides ZWO may or may not be recognized. Please check the kernel version.

- **Wi-Fi / Mount Notes**:
  - **Debian 13 (Trixie)**: Wi-Fi has been tested from Raspberry Pi to the mount. Due to the limited Wi-Fi power management on the Raspberry Pi, it is highly recommended to disable low power mode if you opt to connect via Wi-Fi to the mount. A USB connection to the mount is recommended when possible, as commands are much quicker and more reliable.

## Camera Drivers

### Player One

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Ceres 462M | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Ceres%20462M/) |
| Uranus-C PRO | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Uranus-C%20PRO/) |
| Mars-C II | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Mars-C%20II/) |

<details>
<summary><strong>Player One Driver Notes</strong></summary>

- **SDK**: Player One Camera SDK v3.10.0 (build target)
- **Connection**: USB (requires udev rules `99-player_one_astronomy.rules`)
- **Tested models**: Ceres 462M (uncooled), Mars-C II (uncooled guide camera, IMX290), and Uranus-C PRO (cooled, IMX585) on Linux arm64.
- **Cooling (TEC)**: Capability-gated on the SDK's `POA_COOLER` / `POA_TARGET_TEMP` config attributes. Uncooled cameras report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`. Cooler control (`CoolerOn`, `SetCCDTemperature`, `CoolerPower`) validated on Uranus-C PRO hardware: reaches and holds the target temperature with closed-loop power regulation.
- **Dew Heater / Fan**: Cooled models expose the lens heater and radiator fan two ways — camera custom Actions (`GetHeaterPower`/`SetHeaterPower`/`GetFanPower`/`SetFanPower`, percent) and the **Player One Thermal Switch** device (see Switch Drivers below) for slider control in clients like NINA. Both are runtime-only by design; no setting persists across connects. Note the fan does not auto-vary with temperature — `CoolerOn` turns cooler + fan on and the fan runs at its set power.
- **Pulse guiding**: Capability-gated on `isHasST4Port`. Driver times the pulse duration via `POA_GUIDE_NORTH/SOUTH/EAST/WEST` bool toggles.

</details>

### QHY

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| QHY268C | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/QHY/QHY268C/) |
| miniCam8M | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/QHY/miniCam8M/) |

<details>
<summary><strong>QHY Driver Notes</strong></summary>

- **SDK**: QHY CCD SDK 26.06.04.16 (build target)
- **Connection**: USB (requires udev rules and firmware; see below)
- **Cooler power**: `CanGetCoolerPower` returns false; cooler power reporting is not implemented to avoid SDK timeouts.
- **PulseGuide**: runs the SDK guide call on a detached thread so the initiator returns immediately (ControlQHYCCDGuide blocks for the full pulse duration on real hardware).
- **Cooler/temp SDK calls**: `ControlQHYCCDTemp` and `SetQHYCCDParam` have no SDK-side timeout and can occasionally run well past their typical duration on real hardware. The driver bounds how long disconnect waits on their background workers and detaches rather than blocking indefinitely; the QHY SDK handle is reference-counted so a detached worker can never use a handle after it's been closed.
- **Known issue — Readout mode "Linearity HDR" (index 1) is slow: ~60-75s per frame, regardless of exposure duration.** "Full Resolution" (index 0) is unaffected and always fast. This is expected SDK/firmware behavior, not a driver bug — the exposure watchdog deadline is derived from the actual buffer size (`GetQHYCCDMemLength`) once known, so it scales with the readout mode instead of killing a legitimate in-progress HDR download. Confirmed on real miniCam8M hardware (2026-08-12) via a from-scratch investigation that ruled out every mitigation attempted:
  - Reproduced identically across three independent QHY SDK builds (25.03.24, 25.09.29, 26.06.04) via a standalone repro completely outside this driver.
  - `CONTROL_USBTRAFFIC` (default and forced to 50) and `CONTROL_SPEED` (unavailable on this camera) made no difference.
  - A background "warm-up" capture does **not** prime subsequent captures — confirmed both in a standalone repro and by re-testing this exact driver on real hardware: a second HDR capture immediately after a successful first one is just as slow.
  - `GetQHYCCDEffectiveArea` returns the same dimensions as the total chip size on this camera, so ROI/cropping isn't the cause.
  - Cross-checked against INDIGO's independent `ccd_qhy`/`ccd_qhy2` driver (both the current and the historical 2.0.0.23 release line) on the same physical camera: same SDK, same call sequence, same ~60-75s result on a clean/cold connection. Two early INDIGO runs that session appeared fast (~5s) but were not reproducible on a fresh USB-level reset — an unexplained anomaly, not a usable mitigation.
  - Reported to QHY in case an undocumented parameter exists to control this pacing — if you find one, please open an issue.
- **Tested model**: miniCam8M on Linux arm64
- **ConformU**: 4.5.0 — 0 errors, 0 issues, 0 timing issues

</details>

### SVBONY

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| SV905C2 | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/SVBONY/SV905C2/) |

<details>
<summary><strong>SVBONY Driver Notes</strong></summary>

- **SDK**: SVBONY Camera SDK v1.13.4 (build target) 
- **Connection**: USB (requires udev rules `90-ckusb.rules`)

</details>

### ToupTek

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| GPCMOS01200KPF | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/GPCMOS01200KPF/) |
| GPCMOS02000KPA | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/GPCMOS02000KPA/) |
| ATR2600M (cooled, IMX571) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/ATR2600M/) |

<details>
<summary><strong>ToupTek Driver Notes</strong></summary>

- **SDK**: ToupTek toupcamsdk 2026-01-28 (build target)
- **Connection**: USB (self-contained `libtoupcam.so`; no libusb/libudev link dependency)
- **Tested models**: GPCMOS01200KPF and GPCMOS02000KPA (guide cameras) and ATR2600M (cooled APS-C mono, IMX571) — all ConformU-validated on Linux arm64. Other ToupTek models sharing the same SDK are expected to work but have not been individually verified.
- **ConformU**: 4.3.0 — ATR2600M: 0 errors, 0 issues, 0 timing issues (SDK 59.30701.20260128).
- **Cooling (TEC)**: Capability-gated on `TOUPCAM_FLAG_TEC` / `TOUPCAM_FLAG_TEC_ONOFF`. Uncooled cameras report `CanSetCCDTemperature = false`. On cooled models (ATR2600M) `CoolerOn` / `SetCCDTemperature` / `CoolerPower` drive the TEC; verified reaching −10 °C on hardware.
- **Readout modes (conversion gain + High Full Well)**: on sensors that support them, ASCOM `ReadoutModes` exposes the conversion-gain (`HCG` / `LCG`, plus `HDR` on HDR-capable models) and `High Full Well` hardware modes as a dropdown (e.g. NINA). On the IMX571 these trade read-noise vs full-well (HCG = low noise; LCG / High Full Well = larger full well, ~51 ke⁻ → ~100 ke⁻). Sensors without these keep a single `Normal` mode.
- **Offset**: exposed as ASCOM `Offset` (black level, `TOUPCAM_OPTION_BLACKLEVEL`) on cameras reporting `TOUPCAM_FLAG_BLACKLEVEL`; `OffsetMax` scales with the output bit depth. Cameras without it report `PropertyNotImplemented`.
- **Dew heater / fan / tail LED**: cooled cameras expose an anti-fog dew heater, radiator fan, and the tail indicator LED through the **ToupTek Thermal Switch** device (see Switch Drivers) — `switchType: thermal`, bound by `cameraIndex`, sharing the camera's SDK handle so it runs alongside the camera. Elements are capability-probed per model.
- **Binning**: 1×/2×/3×/4×. Odd bin factors need an even sensor-ROI span, which the driver handles by padding the ROI to even (the SDK floor-bins it back to the requested pixel count).
- **FullWellCapacity**: reported as the ADU saturation value; the true electron full well is a sensor-datasheet figure the SDK does not expose (see readout modes above for the ~51 ke⁻ / ~100 ke⁻ IMX571 modes).

</details>

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| ASI120MM Mini | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI120MM%20Mini/) |
| ASI174MM Mini | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI2600MC Pro | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MM%20Pro/) |
| ASI290MM Mini | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI290MM%20Mini/) |
| ASI462MM | USB |  | pending arm64 re-validation |
| ASI585MC Pro | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI585MC%20Pro/) |
| ASI662MC | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI662MC/) |

<details>
<summary><strong>ZWO Driver Notes</strong></summary>

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.
- **Tested model**: ASI585MC Pro (cooled, IMX585) on Linux arm64.
- **ConformU**: 4.5.0 — ASI585MC Pro: 0 errors, 0 issues, 0 timing issues.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## CoverCalibrator Drivers

### Gemini

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Astro Flat Panel Cover Lite | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Gemini/Astro%20Flat%20Panel%20Cover%20Lite/) |
| Astro Automatic FlatPanel v2 | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Gemini/Astro%20Automatic%20FlatPanel%20v2/) |

<details>
<summary><strong>Gemini CoverCalibrator Driver Notes</strong></summary>

- **Protocol**: reverse-engineered from the vendor's Windows control app (no SDK or published spec). ASCII commands `">X#"`/`">Xnnn#"`, 9600 baud 8N1. Replies are `"*"` + echoed command letter + payload + `"#"` (e.g. `>V#` → `*V206#`); `>S#` is the exception, replying three single-digit flags (`*S111#`) rather than one combined number.
- **Connection**: USB. The panel's controller is an ESP32-class board using Espressif's native USB-serial/JTAG stack (by-id name `usb-Espressif_USB_JTAG_serial_debug_unit_...`), not a CH340/CH341 adapter. Auto-detection scans `/dev/serial/by-id/` for CH340/CH341/generic USB-serial names as well as `Espressif`, then probes with the `>H#` identity handshake. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Cover**: no motorized cover on this model — `OpenCover`/`CloseCover`/`HaltCover` throw `MethodNotImplemented` unconditionally and `CoverState` always reports `NotPresent`.
- **Tested model**: Gemini Astro Flat Panel Cover Lite (firmware 206) on Linux arm64.
- **ConformU**: 4.4.0 — 0 errors, 0 issues, 0 timing issues.

</details>

<details>
<summary><strong>Gemini Astro Automatic FlatPanel v2 Driver Notes</strong></summary>

- **Protocol**: no vendor docs or hardware were available for this model — implemented entirely from INDI's open-source `GeminiFlatpanelRev2Adapter` (`indilib/indi`, `drivers/auxiliary/gemini_flatpanel_adapters.{h,cpp}`). Shares the `">X#"`/`">Xnnn#"` wire syntax and light/brightness commands (`>L#`/`>D#`/`>B<value>#`/`>J#`) with the Lite model above; adds a real motorized cover (`>O#`/`>C#`) and a different `>S#` status reply layout (`*S<2-digit id><motor><light><cover>#`).
- **Connection**: USB, 9600 baud 8N1. Selected via a `flatPanelModel` config field (`"lite"`/`"v2"`) on the same `gemini`+`covercalibrator` router slot as the Lite model; shares port auto-detection with the Lite driver.
- **Cover**: `OpenCover`/`CloseCover` block the wire for up to 30s until the hardware's exact completion reply (`*OOpened#`/`*CClosed#`) arrives, run on a background thread so the ASCOM async initiator returns immediately. `HaltCover` has no hardware equivalent on Rev2 — it stops the driver from *reporting* `Moving` but cannot interrupt the in-flight motor command.
- **Calibrator/cover port contention**: `CalibratorOn`/`CalibratorOff` share the same physical serial link and port-level mutex as `OpenCover`/`CloseCover`. Calling `CalibratorOn` while a cover move is in flight can block behind the cover's up-to-30s wire wait; **fixed by running `CalibratorOn`/`CalibratorOff` on a background thread** (mirroring the cover task) so the ASCOM initiator still returns immediately, with `CalibratorChanging`/`CalibratorState` correctly reporting `NotReady` while the background command is in flight. Caught by ConformU: the first `CalibratorOn` call, issued right after a `HaltCover` on a cover that was still physically moving, blocked the HTTP thread for 6+ seconds before this fix.
- **Tested model**: Gemini Astro Automatic FlatPanel v2 (firmware 408) on Linux arm64.
- **ConformU**: 4.5.0 — 0 errors, 0 issues, 0 timing issues.

</details>

### WandererAstro

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| WandererCover V4 (Pro / EC / EC-IR) | USB/Serial | ✓ | [ConformU Validation](AlpacaCore/conformu/WandererAstro/WandererCover%20V4/) |

<details>
<summary><strong>WandererAstro CoverCalibrator Driver Notes</strong></summary>

- **Protocol**: WandererCover V4 ASCII serial protocol, firmware ≥ 20250405 (no SDK required). Docs in `AlpacaCore/external/WandererAstro/`.
- **Connection**: USB/Serial (CH340 adapter, fixed **19200 baud, 8N1**). Auto-detection supported.
- **Auto-detection**: Scans `/dev/serial/by-id/` for CH340/CH341 USB-serial devices (vendor `1a86`) and listens for the continuously-streamed status frame identifying a `WandererCoverV4` model. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Cover + calibrator**: motorized dust cover (`OpenCover`/`CloseCover`) plus EL flat panel (`CalibratorOn`/`CalibratorOff`, brightness 0–255).
- **HaltCover**: the protocol has no hardware halt command; `HaltCover` stops the driver's move-tracking so `CoverState`/`CoverMoving` immediately stop reporting `Moving` while the cover completes its current travel mechanically.
- **Tested model**: WandererCover V4 Pro (firmware 20250504) on Linux arm64 (Debian 13).
- **ConformU**: 4.3.0 — 0 errors, 0 issues, 0 timing issues.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## Dome Drivers

*No drivers currently available.*

[↑ Back to top](#alpacabridge-supported-drivers)

## FilterWheel Drivers

### Player One

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Phoenix Wheel (PW8) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/PW8/) |

<details>
<summary><strong>Player One FilterWheel Driver Notes</strong></summary>

- **SDK**: Player One FilterWheel SDK v1.2.3 (`libPlayerOnePW`, separate library from the camera SDK)
- **Connection**: USB
- **Tested model**: Phoenix Wheel PW8 (8-position) on Linux arm64
- **ConformU**: 4.3.0 — 0 errors, 0 issues, 0 timing issues
- **Filter names / focus offsets**: per-slot aliases and focus offsets stored on the wheel (set via Player One's own software) are read as defaults at connect; `filterNames` from the AlpacaBridge config overrides them and nothing is written back to the wheel.
- **Position** reports −1 while the wheel is rotating, per the ASCOM IFilterWheelV3 contract.

</details>

### QHY

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| miniCam8M CFW (integrated, 8-slot) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/QHY/miniCam8M%20CFW/) |

<details>
<summary><strong>QHY FilterWheel Driver Notes</strong></summary>

- **SDK**: QHY CCD SDK 26.06.04.16 (shared with the QHY camera driver)
- **Connection**: USB — controlled through the SAME physical handle as its paired QHY camera (not a separately enumerable device); the camera and filter wheel driver share one `OpenQHYCCD` via a reference-counted handle in `QHYSDKWrapper`, and can be connected/disconnected independently.
- **Tested model**: miniCam8M integrated CFW (8-slot) on Linux arm64
- **ConformU**: 4.4.0 — 0 errors, 0 issues, 0 timing issues
- **Position caching**: `GetQHYCCDCFWStatus` is a ~100-130ms hardware round trip on this SDK, which blows the ASCOM FAST (0.1s) target for the first `Position`/`DeviceState` read after `Connect`. The driver does one warm-up read during `Connect` (charged against the 1.0s STANDARD budget) and caches the settled position afterward. The cache is served only when no move is outstanding — while a move is pending, every read stays live and is compared against the commanded target before caching, because this SDK does **not** report a distinct "moving" sentinel the way ToupTek's does: `GetQHYCCDCFWStatus` reports the wheel's actual passing position throughout the physical rotation (including intermediate slots and occasional `-1`) until it settles. A live read taken mid-move that doesn't match the commanded target is masked to `-1` rather than passed through raw — otherwise a client polling during the move can read a real but unrelated transit slot and mistake it for an erroneous arrival — and caching the first post-move reading unconditionally would still freeze `Position` at a stale value and the wheel would never appear to arrive.

</details>

### ToupTek

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| AFW-M (5/7-slot) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/AFW-M/) |

<details>
<summary><strong>ToupTek FilterWheel Driver Notes</strong></summary>

- **SDK**: ToupTek toupcamsdk 2026-01-28 (shared with the ToupTek camera and focuser drivers)
- **Connection**: USB (enumerated by the toupcam SDK via `TOUPCAM_FLAG_FILTERWHEEL`; standalone AFW-M, not a camera-integrated wheel)
- **Tested model**: AFW-M 7-slot on Linux arm64 (wheel firmware `FILTERWHEEL01A_V202_20250903.iic`)
- **ConformU**: 4.3.0 — 0 errors, 0 issues, 0 timing issues
- **Homing at connect**: the driver reads the slot count, writes it back, and homes the wheel (`FILTERWHEEL_POSITION = -1`) at connect — mirroring the INDI toupbase reference driver — so the firmware establishes its slot reference. This is unconditional (matches INDI) and does not depend on a particular firmware; it matters most right after a firmware flash, which clears the slot reference and otherwise leaves the wheel hunting without landing. Expect the wheel to home once on connect.
- **Position** reports −1 while the wheel is rotating, per the ASCOM IFilterWheelV3 contract.

</details>

### WandererAstro

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| SFW36S (8x36mm) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/WandererAstro/SFW36S/) |

<details>
<summary><strong>WandererAstro FilterWheel Driver Notes</strong></summary>

- **Protocol**: ASCII serial over CH340 USB-serial, 19200 8N1 (streamed 'A'-delimited status frames; no SDK)
- **Connection**: USB serial for control; the motor needs the separate 12 V DC input connected (moves silently do nothing without it)
- **Tested model**: SFW36S 8x36mm (reports as `WSFW368`, firmware 20260124) on Linux arm64 — the SFW50/SFW50S (`WSFW508`) share the same 8-slot protocol
- **ConformU**: 4.4.0 — 0 errors, 0 issues, 0 timing issues
- **Minimum firmware**: 20260124 (older firmware predates the status-stream protocol; update via WandererEmpire)
- **Homing at connect**: the driver sends the automatic calibration command (`1500002`) once per connect, matching the vendor's INDI reference — expect the wheel to home once on connect.
- **Position** reports −1 while the wheel is rotating, per the ASCOM IFilterWheelV3 contract.

</details>

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| EFW | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EFW/) |

<details>
<summary><strong>ZWO FilterWheel Driver Notes</strong></summary>

- **SDK**: ZWO EFW SDK Version 1.8.4 (build target)
- **Connection**: USB (requires libusb-1.0)

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## Focuser Drivers

### Astroasis

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Oasis Focuser | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Astroasis/Oasis%20Focuser/) |

<details>
<summary><strong>Astroasis Focuser Driver Notes</strong></summary>

- **Protocol**: Reverse-engineered USB HID vendor protocol (VID:PID 338F:A0F0). No vendor SDK is linked into AlpacaBridge — see `AlpacaCore/external/oasisastro/README.md` for the protocol reference recovered from the vendor's ASCOM driver installer.
- **Connection**: USB HID, via `hidapi`'s hidraw backend.
- **Configuration**: `hidPath` (explicit HID device path) or `focuserIndex` (0-based among enumerated Oasis focusers).
- **Capabilities**: Absolute positioning, halt, max-step/max-increment query, on-board temperature. `StepSize` and temperature compensation are not exposed by the device.
- **ConformU**: 4.4.0 — 0 errors, 0 issues, 0 timing issues on Linux arm64.

</details>

### Gemini

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Gemini Automatic Astro Focuser Pro | USB/Serial | ✓ | [ConformU Validation](AlpacaCore/conformu/Gemini/Astro%20Focuser%20Pro/) |

<details>
<summary><strong>Gemini Focuser Driver Notes</strong></summary>

- **Protocol**: MyFocuserPro2 serial protocol (no SDK required)
- **Connection**: USB/Serial (CH340/CH341 adapter). Auto-detection supported.
- **Auto-detection**: Scans `/dev/serial/by-id/` for CH340/CH341 USB-serial devices and probes with firmware handshake. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.

</details>

### ToupTek

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| AAF (Astro Auto Focuser) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/AAF/) |

<details>
<summary><strong>ToupTek Focuser Driver Notes</strong></summary>

- **SDK**: ToupTek toupcamsdk 2026-01-28 (shared with the ToupTek camera driver)
- **Connection**: USB. Devices are enumerated via `Toupcam_EnumV2` and filtered by `TOUPCAM_FLAG_AUTOFOCUSER`.
- **Configuration**: `focuserIndex` (0-based among AAF devices) or `focuserId` (opaque SDK id; overrides index).
- **Capabilities**: Absolute positioning, halt, max-step query, on-board temperature (tenths of °C), backlash and reverse direction supported by the firmware. `StepSize` is not exposed because the AAF firmware does not report mechanically-valid microns-per-step for arbitrary focuser setups.
- **Temperature compensation**: Not implemented — the AAF action set does not expose a temp-comp control.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues on Linux arm64.

</details>

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| EAF | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EAF/) |

<details>
<summary><strong>ZWO Focuser Driver Notes</strong></summary>

- **SDK**: ZWO EAF Focuser SDK Version 1.7.7 (build target)
- **Connection**: USB (requires libusb-1.0)
- **EAF Pro Bluetooth**: The ZWO EAF Pro Bluetooth version will only currently work with USB connection. Bluetooth support is not yet implemented.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## ObservingConditions Drivers

### WeeWX

| Source | Connection | Linux<br>(arm64) | Status |
|--------|------------|------------------|--------|
| WeeWX HTTP JSON | HTTP(S) | ✓ | [ConformU Validation](AlpacaCore/conformu/WeeWX/) |

<details>
<summary><strong>WeeWX ObservingConditions Driver Notes</strong></summary>

- **Source**: WeeWX HTTP JSON feed (`lcd_datasheet.current`); missing sensors return NaN.
- **Connection**: HTTP(S) to WeeWX REST/JSON endpoint.
- **Configuration**: `weewxUrl` (required), optional `pollIntervalSeconds`, `timeoutMs`.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## Rotator Drivers

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| CAA | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/CAA/) |

<details>
<summary><strong>ZWO Rotator Driver Notes</strong></summary>

- **SDK**: ZWO CAA SDK Version 1.5.9 (build target)
- **Connection**: USB (requires libusb-1.0)

</details>

### WandererAstro

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| WandererRotator Mini (V1 / V2) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/WandererAstro/WandererRotator%20Mini%20V2/) |

<details>
<summary><strong>WandererAstro Rotator Driver Notes</strong></summary>

- **Protocol**: WandererRotator ASCII serial protocol, firmware ≥ 20240226 required (no SDK; protocol reference: INDI `wanderer_rotator_mini`). No published docs — V2 firmware report formats verified against real hardware.
- **Connection**: USB (CH340 adapter, fixed **19200 baud, 8N1**). The motor needs the separate DC power input; the serial link works without it, but moves silently do nothing.
- **Auto-detection**: Scans `/dev/serial/by-id/` for CH340/CH341 USB-serial devices (vendor `1a86`) and probes each with the rotator identity handshake. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Capabilities**: absolute/relative/mechanical moves, halt, hardware reverse, IRotatorV4 Sync (driver-side offset). StepSize is 1/1142°.
- **Tested model**: WandererRotator Mini V2 (firmware 20250222) on Linux arm64 (Debian 13).
- **ConformU**: 4.4.0 — 0 errors, 0 issues, 0 timing issues.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## SafetyMonitor Drivers

*No drivers currently available.*

[↑ Back to top](#alpacabridge-supported-drivers)

## Switch Drivers

### iOptron

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| iMate PowerBox | iMate (OrangePi 3 LTS / H6) | Local GPIO (libgpiod v2) | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/iMate%20PowerBox/) |

<details>
<summary><strong>iOptron Switch Driver Notes</strong></summary>

- **iMate PowerBox** (`vendor: ioptron`, `deviceType: switch`) — the iMate's on-board DC power ports via libgpiod v2 on `/dev/gpiochip1` (override with `gpioChip`). Exposes three switches: `DC3 (always on)` — the hardwired pass-through jack, read-only; `DC1` — GPIO line 118 (PD22); `DC2` — GPIO line 114 (PD18). Ports default to boolean on/off; DC1/DC2 can each opt into 0–100% soft-PWM dimming (per-port `pwm` flag, `pwmFrequencyHz` default 50 Hz) for dew heaters and flat panels — 50 Hz dims panels, confirmed on iMate hardware. Local GPIO only — independent of the iOptron mount RS-232 protocol; runs on the iMate itself under the [OpenAstro](https://github.com/open-astro/aw-flashtool) Armbian image (mainline kernel, Debian 13), which already ships libgpiod v2 plus a `gpio`-group udev rule for `/dev/gpiochip*`. Connecting powers the ports on; disconnecting does not power them off. Setup: [PowerPorts.md](AlpacaCore/PowerPorts.md#ioptron-imate).
  - **ConformU** 4.4.0 — ✓ validated on iMate hardware (Linux arm64; OpenAstro Armbian / mainline kernel, `/dev/gpiochip1`): 0 errors, 0 issues, 0 timing issues. Run against a mixed config (DC1 PWM with the full 0–100% sweep, DC2 boolean, DC3 read-only pass-through) so all three port types were exercised in one pass. Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all per-id properties + TimeStamp present and GET-consistent; slowest member 12 ms. [Report](AlpacaCore/conformu/iOptron/iMate%20PowerBox/Linux-arm64.txt).

</details>

### Player One

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| Thermal Switch (Dew Heater + Fan) | Cooled cameras (Uranus-C PRO) | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Uranus-C%20PRO%20Thermal%20Switch/) |

<details>
<summary><strong>Player One Switch Driver Notes</strong></summary>

- **Thermal Switch** (`vendor: playerone`, `deviceType: switch`) — exposes a cooled Player One camera's dew (lens) heater and radiator fan (`POA_HEATER_POWER` / `POA_FAN_POWER`) as 0–100% multi-value switch elements, giving clients like NINA sliders for runtime control. Binds to the camera by `cameraIndex` and shares the camera's SDK handle via reference-counted open/close, so it can connect alongside the camera device or on its own. The element list is probed per model at connect; connecting against an uncooled camera (no heater, no fan) fails with `NotImplemented`. Heater/fan are deliberately runtime-only — nothing persists across connects, so a heater turned on in December cannot silently re-apply months later. Cooling itself stays on the standard Camera interface (`CoolerOn` / `SetCCDTemperature`); the cooler is intentionally not a switch element.
  - **ConformU** 4.4.0 — ✓ validated on Uranus-C PRO hardware (Linux arm64, Raspberry Pi CM4): 0 errors, 0 issues, 0 timing issues. Full 0–100% heater sweep and the fan's `Minimum: 1` floor (fan cannot run at 0%; `SetSwitchValue(0)` correctly rejected). Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all per-id properties + TimeStamp present and GET-consistent in 10 ms. Heater previously verified by calorimetry (4.3.0 run): at a held −10 °C target, heater 0→100% raised steady-state cooler power ~34%→~44%, symmetric on heater-off. [Report](AlpacaCore/conformu/Player%20One/Uranus-C%20PRO%20Thermal%20Switch/Linux-arm64.txt).

</details>

### ToupTek

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| Thermal Switch (Dew Heater + Fan + Tail LED) | Cooled cameras (ATR2600M) | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/ATR2600M%20Thermal%20Switch/) |
| StellaVita PowerBox | StellaVita (Raspberry Pi CM4 / BCM2711) | Local GPIO (libgpiod v2) | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/StellaVita/) |

<details>
<summary><strong>ToupTek Switch Driver Notes</strong></summary>

- **Thermal Switch** (`vendor: touptek`, `deviceType: switch`, `switchType: thermal`) — exposes a cooled ToupTek camera's anti-fog dew heater (`TOUPCAM_OPTION_HEAT`), radiator fan (`TOUPCAM_OPTION_FAN`), and tail indicator LED (`TOUPCAM_OPTION_TAILLIGHT`, on/off — turn off to avoid light leaks during imaging) as switch elements. Element ranges come from the camera (fan speed `[0, maxfanspeed]` — often a single on/off speed; heater `[0, HEAT_MAX]`); elements are capability-probed per model at connect (heater/fan via `FLAG_HEAT`/`FLAG_FAN`, the tail LED by probing the option). Binds by `cameraIndex` and shares the camera's Toupcam handle via a reference-counted open, so it runs alongside the camera device. The cooler itself stays on the Camera interface (`CoolerOn` / `SetCCDTemperature`), not a switch element.
  - **ConformU** 4.4.0 — ✓ validated on ATR2600M hardware (Linux arm64): 0 errors, 0 issues, 0 timing issues. Three elements exercised: `DewHeater` (0–4), `Fan` (0–1, single-speed on this model), `TailLight` (0–1). Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all per-id properties + TimeStamp present and GET-consistent; slowest member 2 ms. [Report](AlpacaCore/conformu/ToupTek/ATR2600M%20Thermal%20Switch/Linux-arm64.txt).
- **StellaVita PowerBox** (`vendor: touptek`, `deviceType: switch`, `switchType: stellavita`) — the StellaVita's four on-board 12V DC ports via libgpiod v2 on `/dev/gpiochip0` (override with `gpioChip`). Exposes Port 1–4 mapped to BCM GPIO 18/10/17/4 (on BCM2711 the libgpiod line offset equals the BCM GPIO number); mapping verified on hardware against the board's `gpio=18,10,17,4,9,11=op,dh,pu` config.txt directive. GPIO 9/11 power the on-board Cypress USB hub and are deliberately not exposed. All four ports are boolean on/off by default; each can opt into 0–100% soft-PWM dimming (per-port `pwm` flag, `pwmFrequencyHz` default **100 Hz** — tested best on StellaVita, dims flat panels smoothly without 50 Hz flicker). Local GPIO only — independent of the ToupTek camera/focuser SDK; runs on the StellaVita itself (arm64). Connecting preserves the board's boot-high state (ports powered on); disconnecting does not power them off. Setup: [PowerPorts.md](AlpacaCore/PowerPorts.md#touptek-stellavita-raspberry-pi-cm4).
  - **ConformU** 4.4.0 — ✓ validated on StellaVita hardware (Linux arm64; Raspberry Pi CM4, Debian 13 Trixie, `/dev/gpiochip0`): 0 errors, 0 issues, 0 timing issues. Run against a mixed config (Port 1 PWM with the full 0–100% sweep, Ports 2–4 boolean) so both the boolean and soft-PWM paths were exercised in one pass; DeviceState reported all 4 ids + TimeStamp via the shared `SwitchDriver` base in 9 ms. [Report](AlpacaCore/conformu/ToupTek/StellaVita/Linux-arm64.txt).

</details>

### WandererAstro

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| WandererBox Pro V3 Power Box | WandererBox Pro V3 | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/WandererAstro/WandererBox%20Pro%20V3/) |

<details>
<summary><strong>WandererAstro Switch Driver Notes</strong></summary>

- **WandererBox Pro V3** (`vendor: wandererastro`, `deviceType: switch`, `switchType: wandererbox-pro-v3`) — the 11-port power box as 24 Alpaca switches: DC1/DC2 always-on rails (read-only), DC3-4 adjustable output (on/off + 5.0–13.2 V setpoint, 0.1 V steps), DC5/6/7 PWM dew heaters (0–255), DC8-9/DC10-11 switched pairs, five USB power groups, and ten read-only sensor switches (input voltage, three currents, ambient temp/humidity, dew point, three temperature probes; unconnected sensors report -127 °C / 0). Dew-heater auto modes (dew-point/constant-temperature) stay device-side — set them in WandererEmpire; the driver writes manual PWM only, matching the vendor's own ASCOM driver.
  - **Protocol**: WandererBox streamed-status serial protocol at 19200 8N1 (no SDK). Reference: INDI `wandererbox_pro_v3`.
  - **Tested model**: WandererBox Pro V3 (firmware 20250410) on Linux arm64 (Debian 13).
  - **ConformU** 4.4.0 — ✓ 0 errors, 0 issues, 0 timing issues. [Report](AlpacaCore/conformu/WandererAstro/WandererBox%20Pro%20V3/Linux-arm64.txt).

</details>

### ZWO

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |
| ASIAIR Pro 12V Power | ASIAIR Pro (Pi 4) | Local GPIO (libgpiod v2) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASIair%20Pro/) |
| ASIAIR Plus 12V Power | ASIAIR Plus (Pi CM4) | Local GPIO (libgpiod v2) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASIair%20Plus%20(Pi%20CM4)/) |
| ASIAIR Plus 12V Power | ASIAIR Plus (RK3568) | ZWO `pwm_gpio.ko` ioctl | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASIair%20Plus%20(RK3568)/) |

<details>
<summary><strong>ZWO Switch Driver Notes</strong></summary>

- **Dew Heater** (`switchType: dewheater`) — exposed when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Bind to a camera via `cameraIndex` / `cameraId`.
  - **ConformU** 4.4.0 — 0 errors / 0 issues / 0 timing, Linux arm64 (Debian 13 Trixie), ASI2600MM Pro. Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all per-id properties + TimeStamp present and GET-consistent; slowest member 10 ms.
- **ASIAIR Pro 12V Power** (`switchType: asiair`) — four on-board 12V DC ports via libgpiod v2. Default Pi 4 layout: Port 1 = GPIO 12, Port 2 = GPIO 13, Port 3 = GPIO 26, Port 4 = GPIO 18 on `/dev/gpiochip0`. Ports are boolean by default; set `"pwm": true` for a 0–100% soft-PWM channel (default 1 kHz, via `pwmFrequencyHz`). Runs on the ASIAIR itself; arm64-only (re-image the stock 32-bit OS) with the stock `pigpiod`/`zwoair_imager` disabled. Setup: [PowerPorts.md](AlpacaCore/PowerPorts.md).
  - **ConformU** 4.4.0 — 0 errors / 0 issues / 0 timing, Linux arm64 (Raspberry Pi 4, Debian 13 Trixie). Mixed 2 PWM (full 0–100% sweeps) + 2 boolean config. Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all 4 ids × per-id properties + TimeStamp, GET-consistent in 11 ms.
- **ASIAIR Plus 12V Power — Pi CM4** (`switchType: asiair-plus-picm4`) — the CM4 ASIAIR Plus (Raspberry Pi Compute Module 4, BCM2711) shares the Pro's wiring exactly (GPIO 12/13/26/18 on `/dev/gpiochip0`, active-high, default-on), verified against live hardware (GPIO13 read back at 59%, GPIO26 at 34%). Reuses the `asiair` driver and `default_asiair_pro_config()` unchanged — `asiair-plus-picm4` is a thin router/UI alias, not a separate driver. Same arm64 / disable-stock-daemons constraints as the Pro. Setup: [PowerPorts.md](AlpacaCore/PowerPorts.md).
  - **ConformU** 4.4.0 — 0 errors / 0 issues / 0 timing, Linux arm64 (Raspberry Pi CM4, Debian 13 Trixie). Mixed 2 PWM (full 0–100% sweeps) + 2 boolean config. Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all 4 ids × per-id properties + TimeStamp, GET-consistent in 9 ms.
- **ASIAIR Plus 12V Power — RK3568** (`switchType: asiair-plus-rk3568`) — Rockchip RK3568 ASIAIR Plus via ZWO's `pwm_gpio.ko` kernel module on `/dev/pwm-gpio-misc` (reverse-engineered header at `AlpacaCore/external/ZWO/asiair-plus/pwm_gpio.h`). Wrapper indices 0–3 → kernel ioctl indices 4–7 (DC1–DC4). `SET_LEVEL` polarity is inverted (`level=0` ⇒ port ON); the wrapper hides this so ASCOM `value=1` = on. PWM is userspace soft-PWM (default **50 Hz**, matching the stock daemon's `period_ns = 20,000,000`); the module's own hardware-PWM path is unreachable and GPIO bank 4 has no PWM mux. Requires the stock ZWO kernel (4.19.219) to keep `pwm_gpio.ko` loaded, plus the `99-zwo-asiair-plus.rules` udev rule and `gpio`-group membership. Setup: [PowerPorts.md](AlpacaCore/PowerPorts.md).
  - **ConformU** 4.4.0 — 0 errors / 0 issues / 0 timing, Linux arm64 (kernel 4.19.219 + stock `pwm_gpio.ko`). Mixed 1 PWM (full 0–100% sweep through the ioctl soft-PWM) + 3 boolean config. Re-validated on the shared `SwitchDriver` base `DeviceState` (issue #107): all 4 ids × per-id properties + TimeStamp, GET-consistent in 14 ms. This run also proved the fixed `.deb` packaging end-to-end on a stock-kernel box (bundled `99-zwo-asiair-plus.rules` + postinst `gpio` group creation — previously the device was EACCES-dead unless the shell installers had run).

</details>

[↑ Back to top](#alpacabridge-supported-drivers)

## Telescope Drivers

### Celestron

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| CGX-L | USB/Serial (hand controller) | ✓ | [ConformU Validation](AlpacaCore/conformu/Celestron/) |

<details>
<summary><strong>Celestron Driver Notes</strong></summary>

- **Protocol**: NexStar HC serial protocol with MC passthrough (P-command) for per-axis control.
- **Connection**: USB/Serial via hand controller.
- **Required firmware**: Driver is built and tested against **HC (GEM) 5.35.3179** and **MC 7.18.5020**. Other firmware versions are not supported — behavior on earlier or later firmware has not been validated and may differ in slew, tracking, and pier-side semantics.
- **Required HC startup**: Power mount on, press Enter through Switch Position → Location → select **Last Alignment** → Enter. HC must show **"CGX-L Ready"** before connecting the driver. Slews are refused until the mount reports aligned or a `SyncToCoordinates` has been performed in the current driver session.
- **Location/time**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped once the mount is aligned — applying them would invalidate the HC alignment model (especially StarSense). Writes succeed from the client's perspective but do not touch the mount. Set these before completing alignment if you need them to take effect.
- **RA slew offset**: Driver learns a running-average RA undershoot correction and pre-biases subsequent slews (adaptation matches INDI's `SlewOffsetRa`). CGX-L fw 7.18 does not track during a goto, causing consistent RA undershoot without this compensation.
- **Post-slew tracking**: Tracking is re-asserted via the top-level `T` set-tracking-mode command after each slew completes; CGX-L fw 7.18 does not auto-resume tracking after goto.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via the autoguider port. The firmware times the pulse internally — no software sleep or encoder math required. Position hold/correction pattern bridges the gap between the low-level firmware command and ASCOM coordinate expectations.
- **Pier side**: `SideOfPier` reports actual pier side via the HC `p` command (`W` = pierWest, `E` = pierEast).

</details>

### iOptron

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| HEM27 series | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/HEM27) |
| HAE43 series | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/HAE43) |
| HAE29C | USB/Serial | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/HAE29C) |


<details>
<summary><strong>iOptron Driver Notes</strong></summary>

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi (TCP). Auto-detection supported for both — `connectionType: "auto"` scans serial ports first, then falls back to network discovery if no serial mount is found.
- **Serial auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, Silicon Labs, and generic USB-serial devices and probes each with an iOptron `:MountInfo#` query at 115200 baud. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`. The 4-byte model code response (no `#` terminator) is mapped to a human-readable mount name (e.g., `0025` → HEM27) using the current INDI v3 model table.
- **Network auto-detection**: When the connection type is set to Network/Auto, the driver runs a multi-phase discovery: (1) probes well-known iOptron Wi-Fi module addresses (`10.10.100.254`, `10.10.100.1`, `192.168.100.1`) on ports 8899 and 4030; (2) if no mount found, queries the default gateway on each local interface (iOptron mounts act as the AP gateway); (3) if still not found, scans all hosts on each local subnet (up to /24) with parallel non-blocking TCP connect probes. Each candidate is verified with a `:MountInfo#` query before being accepted.
- **Mount identification**: On connect, the driver queries `:MountInfo#` and maps the model code to a name displayed in `Name` (e.g., "iOptron HEM27"), `UniqueID`, and server logs. 60+ models supported including CEM, GEM, HEM, HAE, HAZ, and SkyHunter series.
- **Wi-Fi reliability**: Network (TCP) connections drain stale acknowledgment bytes from blind commands to prevent buffer accumulation on the mount's Wi-Fi module. `IsPulseGuiding` uses lock-free atomics to meet the ConformU fast response target over high-latency links.
- **Tested firmware**: Driver tested on **HEM27** with main board firmware **V240121** and hand controller firmware **V241201**, and on **HAE29C EQ** (model code 0036, current firmware as of 2026-07-14). Other firmware versions and models may work but have not been individually verified.
- **HAE29C firmware quirks**: The driver carries three hardware-verified workarounds gated strictly to model code 0036 (other models are unaffected): (1) `:MP1#` park slews complete physically but never report status 6 — the driver sends `:ST0#` to finalize once the mount is stationary at the park target; (2) a park issued while already at the park position wedges the same way and gets the same finalizer; (3) GOTO stops compensating sidereal motion during its final ~1 s approach, settling ~11–16 arcsec east in RA — the driver closes the residual with a duration-computed pulse-guide trim (up to 3 iterations).
- **ConformU**: HEM27 validated with ConformU 4.3.0 (USB and Wi-Fi); HAE29C validated with ConformU 4.4.0 (USB) — 0 errors, 0 issues, 0 timing violations, on Linux arm64.

</details>

### OnStep

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Generic OnStep (DIY harmonic-drive mount) | USB/Serial | ✓ | [ConformU Validation](AlpacaCore/conformu/OnStep/Generic%20OnStep/) |

<details>
<summary><strong>OnStep Driver Notes</strong></summary>

- **Protocol**: LX200-derived serial protocol, per INDI's `lx200_OnStep` reference driver (no vendor SDK or protocol PDF — OnStep is open-source firmware for DIY/retrofit mounts, commonly run on Arduino Mega/Due, Teensy, or ESP32 boards).
- **Connection**: USB/Serial only in this project (no Wi-Fi/network config exposed). Default 9600 baud, 8N1. Auto-detection scans `/dev/serial/by-id/` (common USB-serial chip vendor IDs plus `Arduino`/`Teensy` substrings) falling back to `/dev/ttyUSB0`–`9` **and** `/dev/ttyACM0`–`9` (OnStep boards commonly enumerate as ttyACM), probing each with the `:GVP#` identity command.
- **Pulse guiding**: Native hardware pulse guide with mount-side timing (`:Mgn####`/`:Mgs####`/`:Mge####`/`:Mgw####`, milliseconds) — no software-timed stop thread required.
- **SideOfPier**: Always computed from hour angle (same convention as iOptron/SynScan/Celestron) — `:GU#`'s `E`/`W` flag reports the mount's *physical* pier orientation, not ASCOM's hour-angle-defined pointing state, and using it directly failed ConformU's SideofPier check on real hardware (see AGENTS.md OnStep notes).
- **Tested model**: Generic OnStep DIY harmonic-drive mount, firmware "On-Step" v10.23a, Linux arm64.
- **ConformU**: 4.5.0 — 0 errors, 0 issues, 0 timing violations (clean run, 2026-08-08, re-validated on a freshly purged/reinstalled SBC after the auto-detect port-search refactor landed on `serial_by_id_scan.h`). PulseGuide ±9 East-West movement occasionally lands 1-2 residual issues right at the 0.07″ tolerance boundary on other runs — documented in AGENTS.md as inherent hardware noise, root-caused via 4 independent tests, not a driver bug — accepted rather than chased with a tolerance/behavior change.

</details>

### SynScan V3/V4

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Sky-Watcher HEQ5 PRO | USB/Serial (hand controller) | ✓ | [ConformU Validation](AlpacaCore/conformu/SynScan/Sky-Watcher%20HEQ5%20PRO/) |

<details>
<summary><strong>SynScan Driver Notes</strong></summary>

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested). Auto-detection supported — `connectionType: "auto"` scans serial ports for SynScan hand controllers and connects to the first responding mount.
- **Auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, and generic USB-serial devices and probes each with a SynScan firmware version query. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Sky-Watcher HEQ5 PRO Firmware**: Hand controller firmware 4.42.00, motor controller firmware 3.46
- **Pulse guiding**: Software-timed variable-rate slew (SynScan has no hardware pulse guide command). Driver issues a variable-rate axis slew at the configured guide rate, times the pulse duration in a background thread, then stops the axis and restores sidereal tracking. GEM pier-side DEC direction flip applied automatically. Position reporting uses accumulated `rate × duration` deltas in the target coordinate frame for ConformU tolerance compliance.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues (pulse guide tested across N/S/E/W at declinations -9, +9, -3, +3).

</details>

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| AM3 | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/AM3/) |
| AM5 | USB/Serial, Wi-Fi |  | pending arm64 re-validation |
| AM5N | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/AM5N/) |
| AM7 | USB/Serial, Wi-Fi |  | pending arm64 re-validation |

<details>
<summary><strong>ZWO Telescope (ASI Mount) Driver Notes</strong></summary>

- **Protocol**: ZWO Mount Serial Communication Protocol (see `AlpacaCore/external/ZWO/AM/ZWO_Mount_Protocol.md`)
- **Connection**: Serial over USB, network (TCP), or **auto-detect**. **Tested and working with USB and WiFi**. PulseGuide and slew behavior validated over both USB and WiFi; timing tuned for high-latency (WiFi) connections. `connectionType: auto` probes USB serial ports and the mount's WiFi AP (`192.168.4.1:4030`) at connect time and connects to whichever ZWO mount answers (AM3/AM5/AM5N/AM7).
- **Tested firmware**: Driver tested on ZWO **firmware 1.8.8\***. Other firmware versions and models (e.g., AM3, AM5, AM7) may work but have not been verified.
- **AM5N validated** (ConformU 4.4.0, Linux arm64, USB): 0 errors, 0 issues, 0 timing issues. Park requires the mount to have been homed; the driver infers park completion from a stationary mount when :hP does not physically move it. Setting Tracking=true while parked throws `InvalidWhileParked`. The park-state cache in the poll thread is left un-pinned during active parks so the completion inference is not defeated.
- **WiFi characteristics**: the same driver connects and operates over the mount's WiFi AP (`192.168.4.1:4030`) with auto-detect. ConformU over WiFi reports 0 errors but a few STANDARD (1.0 s) members (Park, FindHome, guide-rate writes) land at ~1.03–1.09 s because each operation costs several serial round-trips at WiFi latency — a link-speed characteristic, not a driver defect. USB is the ConformU-validated transport; WiFi is verified functional for normal use.

</details>

[↑ Back to top](#alpacabridge-supported-drivers)
