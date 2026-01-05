# AlpacaCore Supported Drivers

This document lists all hardware vendors and device types that are verified to work with AlpacaCore.

**All drivers listed in this document have been verified using the ConformU tool to ensure compliance with the ASCOM Alpaca specification.**

## General Notes

- **ConformU Verification**: All drivers listed above have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Driver Build Guide](docs/development/driver_build.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **USB/Serial**: USB-to-serial adapter or direct serial connection
  - **Ethernet**: Network-based connection (TCP/IP)


## Camera Drivers

No camera drivers have been ConformU-verified yet.

### ZWO

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARM) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|----------------|--------|
| All ASI cameras | USB | | | | | | Not yet verified |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0 on macOS and Linux)
- **Supported Platforms (SDK)**: Windows (x64, x86), macOS (x64, arm64), Linux (x64, x86, armv6, armv7, armv8)
- **Linux USB Permissions**: Install `lib/linux/asi.rules` udev rules for USB device access
- **Verified OS/Arch**: Not yet verified (driver in development)

## Telescope/Mount Drivers

### iOptron

| Model Series | Connection | Windows<br>(x64) | macOS<br>(x64) | macOS<br>(arm64) | Linux<br>(x64) | Linux<br>(ARM) | Status |
|--------------|------------|------------------|----------------|------------------|---------------|----------------|--------|
| CEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |
| GEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |
| HEM series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |
| HAE series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |
| HAZ series | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |
| SkyHunter | USB/Serial, Wi-Fi | | | ✓ | | | [ConformU Validation 20260103](conformu/iOptron/) |

### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Verified OS/Arch**: macOS (arm64) (ConformU validated)
- **USB Driver Requirement**: The PL2303HXD USB driver is required for USB/serial connections on macOS. Driver downloads are available from [Prolific Technology](https://www.prolific.com.tw/en/products/smart-i-o-solution-en/usb-1-1-to-uart-serial-printer/). This is not needed when connecting via Wi-Fi.
