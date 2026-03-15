# AlpacaBridge Supported Drivers

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

This document lists all hardware vendors and device types that are verified to work with AlpacaBridge.

## General Notes

- **ConformU Verification**: All drivers listed below have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Driver Build Guide](https://github.com/open-astro/AlpacaBridge/blob/main/AlpacaCore/docs/development/driver-development.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **Ethernet**: Network-based connection (TCP/IP)
  - **USB/Serial**: USB-to-serial adapter or direct serial connection

- **Why do cameras work without a port but mounts need one?**  
  Cameras (ZWO, QHY, etc.) use **vendor SDKs** that talk to the device over USB using the vendor’s USB protocol and **enumerate devices by type** (e.g. “first ZWO camera”, “camera ID 0”). The SDK hides the actual port; you configure by **camera index** or **camera ID**. Mounts (iOptron, SynScan, ZWO AM5, etc.) connect over **generic serial** (RS-232 over a USB–serial adapter or hand controller). The OS exposes these as plain serial ports (`/dev/ttyUSB0`, etc.) with **no “mount” label**—the app can’t tell which port is the mount. So you must specify the **port path** (e.g. `/dev/ttyUSB0`). Auto-discovery (scanning serial ports and probing for a mount) could be added later but is not implemented today.

- **Linux Notes**:
  - **Linux x64**: Tested and verified on Linux x64. For USB/serial devices (cameras, focusers, filter wheels, mounts), your user account must have access to the serial port. Run `sudo usermod -aG dialout $USER` to add your user to the `dialout` group, then log out and back in (or reboot) for the change to take effect. Without this, connections will fail with "Permission denied" (errno=13). Install the appropriate udev rules from `AlpacaCore/external/` for each ZWO device type.
  - **Kernel 6.17.0-14-generic**: On this kernel version, ZWO EAF focusers and ZWO EFW filter wheels are not currently working; other devices listed in this document continue to operate normally.

- **ZWO SDK Versions** (for reference):
  - **ASI Camera SDK**: Version 1.40 (build target)
  - **ASI Mount**: Serial protocol (ZWO Mount Communication Protocol); no separate SDK
  - **CAA Rotator SDK**: Version 1.5.9 (build target)
  - **EAF Focuser SDK**: Version 1.7.7 (build target)
  - **EFW FilterWheel SDK**: Version 1.8.4 (build target)


## Camera Drivers

### QHY

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| QHY268C | USB | ✓ | ✓ | [ConformU Validation 2026-03-12](AlpacaCore/conformu/QHY/QHY268C/) |

### QHY Driver Notes

- **SDK**: QHY CCD SDK 25.09.29 (build target)
- **Connection**: USB (requires udev rules and firmware; see below)
- **Supported Platforms (SDK)**: Linux (x64, ARM64)
- **Linux udev**: Install udev rules from `AlpacaCore/external/QHY/sdk_<arch>_*/lib/udev/rules.d/` (or equivalent path in the SDK). Only one copy of each rules file should be installed to `/etc/udev/rules.d/`.
- **Linux firmware**: QHY cameras require firmware files in `/lib/firmware/qhy/`. Copy from `AlpacaCore/external/QHY/sdk_<arch>_*/lib/firmware/qhy/` to `/lib/firmware/qhy/`. Use the SDK's own `fxload` from `sdk_<arch>_*/sbin/fxload` and install to `/sbin/fxload` (system `fxload` from apt does not support FX3-based cameras).
- **Cooler power**: `CanGetCoolerPower` returns false; cooler power reporting is not implemented to avoid SDK timeouts.
- **Binding**: Use `cameraId` or `cameraIndex` to bind to the target camera.

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| ASI120MM Mini | USB | ✓ | ✓ | [ConformU Validation 2026-01-06](AlpacaCore/conformu/ZWO/ASI/ASI120MM%20Mini/) |
| ASI174MM Mini | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI2600MC Pro | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI2600MM%20Pro/) |
| ASI290MM Mini | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI290MM%20Mini/) |
| ASI462MM | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI462MM/) |
| ASI662MC | USB | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI662MC/) |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Supported Platforms (SDK)**: Linux (x64, ARM64)
- **Linux USB Permissions**: Install `lib/linux/asi.rules` udev rules for USB device access
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.

## CoverCalibrator Drivers

*No drivers currently available.*

## Dome Drivers

*No drivers currently available.*

## FilterWheel Drivers

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| EFW | USB | ✓ | ✓ | [ConformU Validation 2026-01-10](AlpacaCore/conformu/ZWO/EFW/) |

### ZWO FilterWheel Driver Notes

- **SDK**: ZWO EFW SDK Version 1.8.4 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Supported Platforms (SDK)**: Linux (x64, ARM64)
- **Linux USB Permissions**: Install `lib/efw.rules` udev rules for USB device access
- **Binding**: Use `filterwheelId` or `filterwheelIndex` to bind to the target filter wheel

## Focuser Drivers

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| EAF | USB | ✓ | ✓ | [ConformU Validation 2026-01-06](AlpacaCore/conformu/ZWO/EAF/) |

### ZWO Focuser Driver Notes

- **SDK**: ZWO EAF Focuser SDK Version 1.7.7 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Supported Platforms (SDK)**: Linux (x64, ARM64)
- **Linux USB Permissions**: Install `lib/eaf.rules` udev rules for USB device access
- **EAF Pro Bluetooth**: The ZWO EAF Pro Bluetooth version will only currently work with USB connection. Bluetooth support is not yet implemented.
- **Binding**: Use `focuserId` or `focuserIndex` to bind to the target focuser
- **Verified OS/Arch**: Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

## ObservingConditions Drivers

### WeeWX

| Source | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------|------------|---------------|-----------------|--------|
| WeeWX HTTP JSON | HTTP(S) | ✓ | ✓ | [ConformU Validation 2026-01-28](AlpacaCore/conformu/ObservingConditions/WeeWX/) |

### WeeWX ObservingConditions Driver Notes

- **Source**: WeeWX HTTP JSON feed (`lcd_datasheet.current`); missing sensors return NaN.
- **Connection**: HTTP(S) to WeeWX REST/JSON endpoint.
- **Configuration**: `weewxUrl` (required), optional `pollIntervalSeconds`, `timeoutMs`.

## Rotator Drivers

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| CAA | USB | ✓ | ✓ | [ConformU Validation 2026-01-09](AlpacaCore/conformu/ZWO/CAA/) |

### ZWO Rotator Driver Notes

- **SDK**: ZWO CAA SDK Version 1.5.9 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Supported Platforms (SDK)**: Linux (x64, ARM64)
- **Linux USB Permissions**: Install `lib/linux/caa.rules` udev rules for USB device access
- **Binding**: Use `rotatorId` or `rotatorIndex` to bind to the target rotator

## SafetyMonitor Drivers

*No drivers currently available.*

## Switch Drivers

### ZWO

| Device Type | Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|-------------|--------------|------------|---------------|-----------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI2600MC%20Pro/switch-dew%20heater/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | ✓ | ✓ | [ConformU Validation 2026-01-05](AlpacaCore/conformu/ZWO/ASI/ASI2600MM%20Pro/switch-dew%20heater/) |

### ZWO Switch Driver Notes

- **Device Type**: Dew Heater (`switchType: dewheater`)
- **Connection**: Exposed as a Switch device when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`
- **Binding**: Use `cameraId` or `cameraIndex` to bind to the target camera

## Telescope Drivers

### iOptron

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| CEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |
| GEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |
| HAE series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |
| HAZ series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |
| HEM series | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |
| SkyHunter | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-01-03](AlpacaCore/conformu/iOptron/) |

### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi
- **USB/Serial**: For USB/serial connections, ensure your user has access to the serial port (e.g., `dialout` group). Wi-Fi connections do not require additional drivers.

### SynScan

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| SynScan V3/V4 | USB/Serial (hand controller) | ✓ | ✓ | [ConformU Validation 2026-02-04](AlpacaCore/conformu/SynScan/) |

### SynScan Driver Notes

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested)
- **Tested Mount**: Orion Atlas EQ-G

### ZWO

| Model Series | Connection | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|---------------|-----------------|--------|
| AM5 / AM5N | USB/Serial, Wi-Fi | ✓ | ✓ | [ConformU Validation 2026-02-25](AlpacaCore/conformu/ZWO/AM5N/) |

### ZWO Telescope (ASI Mount) Driver Notes

- **Protocol**: ZWO Mount Serial Communication Protocol (see `AlpacaCore/external/ZWO/AM/ZWO_Mount_Protocol.md`)
- **Connection**: Serial over USB or network (TCP). **Tested and working with USB and WiFi** on AM5N.
- **Tested firmware**: Driver tested on **firmware 1.8.8** for the **AM5N**. Other firmware versions and models (e.g., AM3, AM5, AM7) may work but have not been verified.
- **AM5N status**: PulseGuide and slew behavior validated over both USB and WiFi; timing tuned for high-latency (WiFi) connections.
