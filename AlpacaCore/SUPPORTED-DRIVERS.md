# AlpacaCore Supported Drivers

This document lists all hardware vendors and device types that are verified to work with AlpacaCore.

**All drivers listed in this document have been verified using the ConformU tool to ensure compliance with the ASCOM Alpaca specification.**

## General Notes

- **ConformU Verification**: All drivers listed below have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Driver Build Guide](docs/development/driver_build.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **USB/Serial**: USB-to-serial adapter or direct serial connection
  - **Ethernet**: Network-based connection (TCP/IP)

- **Windows Notes**:
  - **Windows 11 x64**: Tested and verified on Windows 11 x64. USB devices typically work without additional drivers as Windows includes native USB support. For serial connections, ensure appropriate USB-to-serial drivers are installed if needed.

- **ZWO SDK Versions** (for reference):
  - **ASI Camera SDK**: Version 1.40 (build target)
  - **EAF Focuser SDK**: Version 1.7.7 (build target)
  - **CAA Rotator SDK**: Version 1.5.9 (build target)
  - **EFW FilterWheel SDK**: Version 1.8.4 (build target)


## Camera Drivers

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| ASI120MM Mini | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-06](conformu/ZWO/ASI/ASI120MM%20Mini/) |
| ASI174MM Mini | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI290MM Mini | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI290MM%20Mini/) |
| ASI462MM | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI462MM/) |
| ASI662MC | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI662MC/) |
| ASI2600MC Pro | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MM%20Pro/) |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Windows Driver Requirement**: The ZWO ASI Camera driver must be installed from ZWO for Windows systems. Download the driver from the [ZWO website](https://www.zwoastro.com/software/).
- **Linux USB Permissions**: Install `lib/linux/asi.rules` udev rules for USB device access
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64), Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

## CoverCalibrator Drivers

*No drivers currently available.*

## Dome Drivers

*No drivers currently available.*

## FilterWheel Drivers

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| EFW | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-10](conformu/ZWO/EFW/) |

### ZWO FilterWheel Driver Notes

- **SDK**: ZWO EFW SDK Version 1.8.4 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Linux USB Permissions**: Install `lib/efw.rules` udev rules for USB device access
- **Binding**: Use `filterwheelId` or `filterwheelIndex` to bind to the target filter wheel
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64), Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

## Focuser Drivers

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| EAF | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-06](conformu/ZWO/EAF/) |

### ZWO Focuser Driver Notes

- **SDK**: ZWO EAF Focuser SDK Version 1.7.7 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Linux USB Permissions**: Install `lib/eaf.rules` udev rules for USB device access
- **EAF Pro Bluetooth**: The ZWO EAF Pro Bluetooth version will only currently work with USB connection. Bluetooth support is not yet implemented.
- **Binding**: Use `focuserId` or `focuserIndex` to bind to the target focuser
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64), Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

## ObservingConditions Drivers

*No drivers currently available.*

## Rotator Drivers

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| CAA | USB | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-09](conformu/ZWO/CAA/) |

### ZWO Rotator Driver Notes

- **SDK**: ZWO CAA SDK Version 1.5.9 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Linux USB Permissions**: Install `lib/linux/caa.rules` udev rules for USB device access
- **Binding**: Use `rotatorId` or `rotatorIndex` to bind to the target rotator
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64), Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

## SafetyMonitor Drivers

*No drivers currently available.*

## Switch Drivers

### ZWO

| Device Type | Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|-------------|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MC%20Pro/switch-dew%20heater/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MM%20Pro/switch-dew%20heater/) |

### ZWO Switch Driver Notes

- **Device Type**: Dew Heater (`switchType: dewheater`)
- **Connection**: Exposed as a Switch device when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`
- **Binding**: Use `cameraId` or `cameraIndex` to bind to the target camera
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64) (ConformU validated)

## Telescope Drivers

### iOptron

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| CEM series | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| GEM series | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HEM series | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HAE series | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HAZ series | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| SkyHunter | USB/Serial, Wi-Fi | ✓ | | ✓ | | ✓ | [ConformU Validation 2026-01-03](conformu/iOptron/) |

### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi
- **USB Driver Requirement**: The PL2303HXD USB driver is required for USB/serial connections on macOS. Driver downloads are available from [Prolific Technology](https://www.prolific.com.tw/en/products/smart-i-o-solution-en/usb-1-1-to-uart-serial-printer/). This is not needed when connecting via Wi-Fi. On Windows, USB-to-serial drivers are typically included with the OS or automatically installed.
- **Verified OS/Arch**: Windows 11 (x64), macOS (arm64), Linux ARMv8 (e.g., Raspberry Pi 5) (ConformU validated)
- **Linux ARM Testing**: Linux ARMv8 (e.g., Raspberry Pi 5) has been tested and verified. ARMv6 and ARMv7 are not tested.

### SynScan

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARMv8) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|-----------------|--------|
| SynScan V3/V4 | USB/Serial (hand controller) | | | ✓ | | | [ConformU Validation 2026-02-04](conformu/SynScan/) |

### SynScan Driver Notes

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested)
- **Tested Mount**: Orion Atlas EQ-G
- **Verified OS/Arch**: macOS (arm64) only (ConformU validated)
