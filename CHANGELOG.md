# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-01-XX

### Added

#### Project Infrastructure
- Complete directory structure following AlpacaCore architecture guidelines
- Root documentation files (LICENSE, README.md, CONTRIBUTING.md, VERSION)
- CMake build system with C++20 support
- Build options for tests and vendor support (ZWO, QHY, iOptron)
- .gitignore for build artifacts and IDE files
- Test infrastructure with Catch2/doctest support
- Example directory structure (minimal_camera, minimal_telescope, simulate_all_devices)

#### Core Utilities
- **Logging system** (`util/logging.h`): Thread-safe, dependency-free logging with configurable sinks
  - Log levels: Trace, Debug, Info, Warn, Error, Critical
  - Default stderr logger with timestamps
  - Convenience macros (ALPACA_LOG_INFO, etc.)
- **Error handling** (`util/error_handling.h`): AlpacaException base class for all errors
- **Threading utilities** (`util/threading.h`): Namespace for future threading primitives
- **Unit conversion** (`util/units.h`): Conversion functions for degrees/radians, hours/degrees (RA), microns/meters

#### Core Alpaca Definitions
- **Device types**: All 11 ASCOM Alpaca device types
  - Camera, Telescope, FilterWheel, Focuser, Rotator, Dome, Shutter, Switch
  - CoverCalibrator, ObservingConditions, SafetyMonitor
- **AlpacaResponseHeader**: Structure for transaction IDs and error information
- **ConnectionState**: Enum for device connection states
- **AlpacaError namespace**: All standard ASCOM Alpaca error codes

#### Base Driver Interface
- **AlpacaDriver** (`alpacadriver.h`): Pure virtual base interface for all devices
  - Common properties: DeviceNumber, Name, UniqueID, Description, DriverInfo, DriverVersion, InterfaceVersion
  - Connection management: Connected (get/set)
  - Action support: SupportedActions, Action(), CanAction()
  - Command methods: CommandBlind(), CommandBool(), CommandString()

#### Device Driver Interfaces
All device types have pure virtual interfaces following ASCOM Alpaca specification:
- **CameraDriver**: Exposure control, binning, sensor properties, cooling, subframe support
- **TelescopeDriver**: Slew, sync, park, tracking, guide rates, site information
- **FilterWheelDriver**: Position control, focus offsets, filter names
- **FocuserDriver**: Absolute/relative positioning, temperature compensation
- **RotatorDriver**: Position control, mechanical position, sync
- **DomeDriver**: Azimuth control, shutter control, park, sync
- **ShutterDriver**: Open/close shutter control
- **SwitchDriver**: Multi-switch support with names, descriptions, value ranges
- **CoverCalibratorDriver**: Cover and calibrator brightness control
- **ObservingConditionsDriver**: Environmental sensors (temperature, humidity, pressure, wind, sky conditions)
- **SafetyMonitorDriver**: Safety condition monitoring

#### Management API
- **ManagementDriver** (`managementdriver.h`): Server management interface
  - Server information: Name, Version, Description, Manufacturer, Location, UTC offset
  - API version support: GetApiVersions()
  - Device discovery: GetConfiguredDevices()
- **DeviceCapabilities**: Structure for device metadata
- Placeholder implementations for discovery service and device registry

#### Build System
- Root CMakeLists.txt with proper C++20 configuration
- Compiler warnings as errors (MSVC /W4, GCC/Clang -Wall -Wextra -Wpedantic)
- Test CMakeLists.txt with Catch2/doctest detection
- Example CMakeLists.txt structure
- Vendor-specific CMakeLists.txt placeholders

#### Tests
- Unit test framework setup
- Test files for camera, telescope, management, and units
- Units conversion tests with comprehensive coverage

### Notes
- All interfaces are pure virtual - concrete implementations will be provided by vendor-specific drivers
- No HTTP/REST/JSON parsing - this is a protocol-level library only
- All source files include proper SSPL v1 license headers
- Follows ASCOM Alpaca API specification from https://ascom-standards.org/api/

