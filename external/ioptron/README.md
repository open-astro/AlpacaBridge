# iOptron Mount Resources

This directory contains iOptron mount resources and documentation.

## Files

- **RS-232_Command_Language2014V310.md** - iOptron RS-232 Command Language specification
  - Version: 3.10
  - Date: January 4th, 2021
  - Complete command reference for iOptron mounts
  - Applies to: CEM120, CEM70, GEM45, CEM40, GEM28, CEM26 series
  - Used as reference for implementing the AlpacaCore iOptron driver

## Connection Information

iOptron mounts support two connection methods:

1. **USB Serial** - Direct serial port connection
   - Standard baud rate: 9600
   - Port path: `/dev/ttyUSB0` (Linux), `COM3` (Windows), etc.

2. **WiFi TCP** - Network connection
   - Default ports vary by model:
     - CEM60-EC: Port 4030
     - HEM27: Port 8899
   - Connect to mount's IP address

## Driver Implementation

The AlpacaCore iOptron driver implements the RS-232 protocol as documented in this specification.

**Driver Files:**
- `include/alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h` - Protocol wrapper API
- `src/vendors/ioptron/ioptron_protocol_wrapper.cpp` - Protocol implementation
- `src/vendors/ioptron/ioptron_telescope_driver.cpp` - Telescope driver implementation

## Building

Build with iOptron support:
```bash
cmake .. -DALPACACORE_ENABLE_IOPTRON=ON
cmake --build .
```

