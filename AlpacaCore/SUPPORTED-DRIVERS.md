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


## Camera Drivers

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARM) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|----------------|--------|
| ASI174MM Mini | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI290MM Mini | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI290MM%20Mini/) |
| ASI462MM | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI462MM/) |
| ASI662MC | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI662MC/) |
| ASI2600MC Pro | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MM%20Pro/) |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Linux USB Permissions**: Install `lib/linux/asi.rules` udev rules for USB device access
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.
- **Verified OS/Arch**: macOS (arm64) (ConformU validated)

## Switch Drivers

### ZWO

| Device Type | Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARM) | Status |
|-------------|--------------|------------|------------------|----------------|------------------|---------------|----------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MC%20Pro/switch-dew%20heater/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | | | ✓ | | | [ConformU Validation 2026-01-05](conformu/ZWO/ASI/ASI2600MM%20Pro/switch-dew%20heater/) |

### ZWO Switch Driver Notes

- **Device Type**: Dew Heater (`switchType: dewheater`)
- **Connection**: Exposed as a Switch device when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`
- **Binding**: Use `cameraId` or `cameraIndex` to bind to the target camera
- **Verified OS/Arch**: macOS (arm64) (ConformU validated)

## Telescope/Mount Drivers

### iOptron

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARM) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|----------------|--------|
| CEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| GEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HAE series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| HAZ series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |
| SkyHunter | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 2026-01-03](conformu/iOptron/) |

### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi
- **USB Driver Requirement**: The PL2303HXD USB driver is required for USB/serial connections on macOS. Driver downloads are available from [Prolific Technology](https://www.prolific.com.tw/en/products/smart-i-o-solution-en/usb-1-1-to-uart-serial-printer/). This is not needed when connecting via Wi-Fi.
- **Verified OS/Arch**: macOS (arm64) (ConformU validated)
