# AlpacaCore Supported Drivers

This document lists all hardware vendors and device types that are verified to work with AlpacaCore.

**All drivers listed in this document have been verified using the ConformU tool to ensure compliance with the ASCOM Alpaca specification.**

## Telescope/Mount Drivers

| Vendor | Models | Connection | Status |
|--------|--------|------------|--------|
| **iOptron** | CEM, GEM, HEM, HAE, HAZ series, SkyHunter | USB/Serial, Ethernet | In testing phase with ConformU |

## Notes

- **ConformU Verification**: All drivers listed above have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.

- **Connection Types**:
  - **USB/Serial**: USB-to-serial adapter or direct serial connection
  - **Ethernet**: Network-based connection (TCP/IP)

- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.

- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Driver Build Guide](docs/development/driver_build.md) for details. All drivers must pass ConformU verification before being added to this list.
