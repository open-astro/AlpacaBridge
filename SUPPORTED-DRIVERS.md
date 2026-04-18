# AlpacaBridge Supported Drivers

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

## Updated 2026-04-16
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

### iOptron

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| CEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |
| GEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |
| HAE series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |
| HAZ series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |
| HEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |
| SkyHunter | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/) |

### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi
- **Tested firmware**: Drivers test on **firmware V241201**. Other firmware versions and models may work but have not been verified.

### SynScan V3/V4

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| Sky-Watcher HEQ5 PRO | USB/Serial (hand controller) | ✓ | ✓ | [ConformU Validation](AlpacaCore/conformu/SynScan/Sky-Watcher%20HEQ5%20PRO/) |

### SynScan Driver Notes

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested)
- **Sky-Watcher HEQ5 PRO Firmware**: Hand controller firmware 4.42.00, motor controller firmware 3.46

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
