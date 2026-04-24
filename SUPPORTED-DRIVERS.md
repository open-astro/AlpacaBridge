# AlpacaBridge Supported Drivers

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

## Updated 2026-04-23
This document lists all hardware vendors and device types that are verified to work with AlpacaBridge.

## General Notes

- **ConformU Verification**: All drivers listed below have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Driver Build Guide](https://github.com/open-astro/AlpacaBridge/blob/main/AlpacaCore/docs/development/driver-development.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **Ethernet**: Network-based connection (TCP/IP)
  - **USB/Serial**: USB-to-serial adapter or direct serial connection

- **Linux Notes**:
  - **Debian 13 (Trixie)**: All drivers have been tested using Debian 13 on x64 and arm64 using ConformU v4.2.1
  - **Kernel 6.12.75-v8-16+ or higher.**: Note: kernel 6.12.75-v8-16+ is required to ensure ZWO EAF/EFW hardware compatibility. Without it, devices besides ZWO may or may not be recognized. Please check the kernel version.

- **Wi-Fi / Mount Notes**:
  - **Debian 13 (Trixie)**: Wi-Fi has been tested from Raspberry Pi to the mount. Due to the limited Wi-Fi power management on the Raspberry Pi, it is highly recommended to disable low power mode if you opt to connect via Wi-Fi to the mount. Even with a NUC running Debian 13, it is recommended to use a USB connection to the mount when possible, as commands are much quicker and more reliable.

## Camera Drivers

### Player One

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| Ceres 462M | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Ceres%20462M/) |

### Player One Driver Notes

- **SDK**: Player One Camera SDK v3.10.0 (build target)
- **Connection**: USB (requires udev rules `99-player_one_astronomy.rules`)
- **Tested model**: Ceres 462M (uncooled) on Linux arm64. x64 validation pending.
- **Cooling (TEC)**: Capability-gated on the SDK's `POA_COOLER` / `POA_TARGET_TEMP` config attributes. Uncooled cameras report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`. Cooler control paths (`CoolerOn`, `SetCCDTemperature`, `CoolerPower`) are implemented but untested against physical cooled hardware.
- **Dew Heater**: Not exposed. The SDK advertises `POA_HEATER_POWER` on cooled models, but we do not currently have a cooled Player One camera available to implement and validate the Switch device. Will be added when hardware is available.
- **Pulse guiding**: Capability-gated on `isHasST4Port`. Driver times the pulse duration via `POA_GUIDE_NORTH/SOUTH/EAST/WEST` bool toggles.

### QHY

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| QHY268C | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/QHY/QHY268C/) |

### QHY Driver Notes

- **SDK**: QHY CCD SDK 25.09.29 (build target)
- **Connection**: USB (requires udev rules and firmware; see below)
- **Cooler power**: `CanGetCoolerPower` returns false; cooler power reporting is not implemented to avoid SDK timeouts.

### SVBONY

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| SV905C2 | USB | ✓ |  ✓ | [ConformU Validation](AlpacaCore/conformu/SVBONY/SV905C2/) |

### SVBONY Driver Notes

- **SDK**: SVBONY Camera SDK v1.13.4 (build target) 
- **Connection**: USB (requires udev rules `90-ckusb.rules`)

### ToupTek

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| GPCMOS01200KPF | USB | ✓ | ✓  | [ConformU Validation](AlpacaCore/conformu/ToupTek/GPCMOS01200KPF/) |

### ToupTek Driver Notes

- **SDK**: ToupTek toupcamsdk 2026-01-28 (build target)
- **Connection**: USB (self-contained `libtoupcam.so`; no libusb/libudev link dependency)
- **Tested model**: GPCMOS01200KPF (guide camera) on Linux x64. Other ToupTek models sharing the same SDK are expected to work but have not been individually ConformU-verified.
- **Dew Heater**: Not supported. The ToupTek SDK exposes `TOUPCAM_FLAG_HEAT` / `TOUPCAM_OPTION_HEAT` for anti-fog heating on cooled cameras, but we do not currently have a cooled ToupTek camera available to implement and validate the Switch device. Will be added when hardware is available.
- **Cooling (TEC)**: Capability-gated on the SDK's `TOUPCAM_FLAG_TEC` / `TOUPCAM_FLAG_TEC_ONOFF` flags. Uncooled cameras report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`. Cooler control paths are implemented but untested against physical cooled hardware.

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| ASI120MM Mini | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI120MM%20Mini/) |
| ASI174MM Mini | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI2600MC Pro | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MM%20Pro/) |
| ASI290MM Mini | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI290MM%20Mini/) |
| ASI462MM | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI462MM/) |
| ASI662MC | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI662MC/) |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.

## CoverCalibrator Drivers

*No drivers currently available.*

## Dome Drivers

*No drivers currently available.*

## FilterWheel Drivers

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| EFW | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EFW/) |

### ZWO FilterWheel Driver Notes

- **SDK**: ZWO EFW SDK Version 1.8.4 (build target)
- **Connection**: USB (requires libusb-1.0)

## Focuser Drivers

### Gemini

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| Gemini Automatic Astro Focuser Pro | USB/Serial | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/Gemini/Astro%20Focuser%20Pro/) |

### Gemini Focuser Driver Notes

- **Protocol**: MyFocuserPro2 serial protocol (no SDK required)
- **Connection**: USB/Serial (CH340/CH341 adapter). Auto-detection supported.
- **Auto-detection**: Scans `/dev/serial/by-id/` for CH340/CH341 USB-serial devices and probes with firmware handshake. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| EAF | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EAF/) |

### ZWO Focuser Driver Notes

- **SDK**: ZWO EAF Focuser SDK Version 1.7.7 (build target)
- **Connection**: USB (requires libusb-1.0)
- **EAF Pro Bluetooth**: The ZWO EAF Pro Bluetooth version will only currently work with USB connection. Bluetooth support is not yet implemented.

## ObservingConditions Drivers

### WeeWX

| Source | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------|------------|---------------|-----------------|--------|
| WeeWX HTTP JSON | HTTP(S) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/WeeWX/) |

### WeeWX ObservingConditions Driver Notes

- **Source**: WeeWX HTTP JSON feed (`lcd_datasheet.current`); missing sensors return NaN.
- **Connection**: HTTP(S) to WeeWX REST/JSON endpoint.
- **Configuration**: `weewxUrl` (required), optional `pollIntervalSeconds`, `timeoutMs`.

## Rotator Drivers

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| CAA | USB | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/CAA/) |

### ZWO Rotator Driver Notes

- **SDK**: ZWO CAA SDK Version 1.5.9 (build target)
- **Connection**: USB (requires libusb-1.0)

## SafetyMonitor Drivers

*No drivers currently available.*

## Switch Drivers

### ZWO

| Device Type | Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|-------------|--------------|------------|---------------|-----------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |

### ZWO Switch Driver Notes

- **Device Type**: Dew Heater (`switchType: dewheater`)
- **Connection**: Exposed as a Switch device when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`

## Telescope Drivers

### Celestron

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| CGX-L | USB/Serial (hand controller) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/Celestron/) |

### Celestron Driver Notes

- **Protocol**: NexStar HC serial protocol with MC passthrough (P-command) for per-axis control.
- **Connection**: USB/Serial via hand controller.
- **Required firmware**: Driver is built and tested against **HC (GEM) 5.35.3179** and **MC 7.18.5020**. Other firmware versions are not supported — behavior on earlier or later firmware has not been validated and may differ in slew, tracking, and pier-side semantics.
- **Required HC startup**: Power mount on, press Enter through Switch Position → Location → select **Last Alignment** → Enter. HC must show **"CGX-L Ready"** before connecting the driver. Slews are refused until the mount reports aligned or a `SyncToCoordinates` has been performed in the current driver session.
- **Location/time**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped once the mount is aligned — applying them would invalidate the HC alignment model (especially StarSense). Writes succeed from the client's perspective but do not touch the mount. Set these before completing alignment if you need them to take effect.
- **RA slew offset**: Driver learns a running-average RA undershoot correction and pre-biases subsequent slews (adaptation matches INDI's `SlewOffsetRa`). CGX-L fw 7.18 does not track during a goto, causing consistent RA undershoot without this compensation.
- **Post-slew tracking**: Tracking is re-asserted via the top-level `T` set-tracking-mode command after each slew completes; CGX-L fw 7.18 does not auto-resume tracking after goto.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via the autoguider port. The firmware times the pulse internally — no software sleep or encoder math required. Position hold/correction pattern bridges the gap between the low-level firmware command and ASCOM coordinate expectations.
- **Pier side**: `SideOfPier` reports actual pier side via the HC `p` command (`W` = pierWest, `E` = pierEast).

### iOptron

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| HEM27 series | USB/Serial, Wi-Fi | ✓ |  | [ConformU Validation](AlpacaCore/conformu/iOptron/HEM27) |
| HAE29 series | USB/Serial, Wi-Fi |  |  | [ConformU Validation](AlpacaCore/conformu/iOptron/HAE29) |


### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi. Auto-detection supported — `connectionType: "auto"` scans serial ports for iOptron mounts and connects to the first responding mount.
- **Auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, Silicon Labs, and generic USB-serial devices and probes each with an iOptron `:MountInfo#` query at 115200 baud. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`. The 4-byte model code response (no `#` terminator) is mapped to a human-readable mount name (e.g., `0025` → HEM27) using the current INDI v3 model table.
- **Mount identification**: On connect, the driver queries `:MountInfo#` and maps the model code to a name displayed in `Name` (e.g., "iOptron HEM27"), `UniqueID`, and server logs. 60+ models supported including CEM, GEM, HEM, HAE, HAZ, and SkyHunter series.
- **Tested firmware**: Driver tested on **HEM27** with main board firmware **V240121** and hand controller firmware **V241201**. Other firmware versions and models may work but have not been individually verified.

### SynScan V3/V4

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| Sky-Watcher HEQ5 PRO | USB/Serial (hand controller) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/SynScan/Sky-Watcher%20HEQ5%20PRO/) |

### SynScan Driver Notes

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested). Auto-detection supported — `connectionType: "auto"` scans serial ports for SynScan hand controllers and connects to the first responding mount.
- **Auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, and generic USB-serial devices and probes each with a SynScan firmware version query. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Sky-Watcher HEQ5 PRO Firmware**: Hand controller firmware 4.42.00, motor controller firmware 3.46
- **Pulse guiding**: Software-timed variable-rate slew (SynScan has no hardware pulse guide command). Driver issues a variable-rate axis slew at the configured guide rate, times the pulse duration in a background thread, then stops the axis and restores sidereal tracking. GEM pier-side DEC direction flip applied automatically. Position reporting uses accumulated `rate × duration` deltas in the target coordinate frame for ConformU tolerance compliance.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues (pulse guide tested across N/S/E/W at declinations -9, +9, -3, +3).

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| AM3 | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU](AlpacaCore/conformu/ZWO/AM3/) |
| AM5 | USB/Serial, Wi-Fi |  |  | [ConformU](AlpacaCore/conformu/ZWO/AM5/) |
| AM5N | USB/Serial, Wi-Fi |  |  | [ConformU](AlpacaCore/conformu/ZWO/AM5N/) |

### ZWO Telescope (ASI Mount) Driver Notes

- **Protocol**: ZWO Mount Serial Communication Protocol (see `AlpacaCore/external/ZWO/AM/ZWO_Mount_Protocol.md`)
- **Connection**: Serial over USB or network (TCP). **Tested and working with USB and WiFi**. PulseGuide and slew behavior validated over both USB and WiFi; timing tuned for high-latency (WiFi) connections.
- **Tested firmware**: Driver tested on ZWO **firmware 1.8.8***. Other firmware versions and models (e.g., AM3, AM5, AM7) may work but have not been verified.
