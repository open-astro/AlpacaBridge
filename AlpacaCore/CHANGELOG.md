# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-01-XX

## [0.2.0] - 2025-01-XX

### Added
- **DeviceRegistry** (`include/alpacacore/device_registry.h`): Complete device management system
  - Singleton pattern for global device registry
  - Thread-safe device registration and lookup
  - Support for registering devices by type and number
  - Device capability enumeration for management API
  - Integration with `get_all_device_capabilities()` function

### Changed
- Updated `device_capabilities.cpp` to use DeviceRegistry instead of placeholder implementation
- Updated license URL in all source files from MongoDB reference to GitHub repository location
  - Changed from `https://www.mongodb.com/licensing/server-side-public-license`
  - Changed to `https://github.com/open-astro/AlpacaCore/blob/main/LICENSE`
  - Updated 49 files (headers, sources, and tests)

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
- **DeviceRegistry** (`device_registry.h`): Singleton device registry for managing all registered devices
  - Thread-safe device registration and retrieval
  - Support for querying devices by type and number
  - Device capability enumeration
  - Integration point for higher-level servers (e.g., AlpacaHTTP)
- Placeholder implementations for discovery service

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
 
## [0.3.0] - 2025-12-16

### Added
- **iOptron Telescope Driver** (`include/alpacacore/vendor/ioptron/ioptron_telescope_driver.h`, `src/vendors/ioptron/ioptron_telescope_driver.cpp`)
  - Concrete `TelescopeDriver` implementation for iOptron mounts
  - Connection management via Alpaca `Connected` property
  - Initial command support for position, motion, and status queries (work in progress)
- **iOptron Protocol Wrapper** (`include/alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h`, `src/vendors/ioptron/ioptron_protocol_wrapper.cpp`)
  - Clean C++ wrapper around iOptron RS‑232 command set over serial or TCP
  - Platform‑specific transport handling (POSIX / Windows) hidden behind PIMPL
  - Helpers for RA/Dec conversion, Alt/Az, site info, and mount status
- **Vendor Documentation** (`external/README.md`, `external/ioptron/README.md`)
  - Guidance for placing vendor SDKs / protocol docs under `external/`
  - iOptron RS‑232 command reference linked for driver development
- **Comprehensive Developer Documentation** (`docs/README.md`, `docs/building/*.md`, `docs/development/*.md`, `docs/getting-started/installation.md`)
  - Central documentation index with organized structure
  - Complete building guide with prerequisites, build options, and troubleshooting
  - Driver development guide describing three‑layer architecture and AI‑assisted workflow
  - Testing guide with comprehensive coverage
  - Architecture documentation explaining design principles
  - Installation guide for all platforms
- **Telescope Driver Enhancement**
  - Added `get_tracking_rates()` method to `TelescopeDriver` interface
  - Returns collection of supported DriveRates (Sidereal, Lunar, Solar, King)
  - Implemented in iOptron telescope driver

### Changed
- **README.md**
  - Enhanced with AI-assisted development information and workflow
  - Added comprehensive driver building guide section
  - Improved documentation links and structure
  - Added quick start building instructions
- **Build System**
  - Added `ALPACACORE_ENABLE_ALL_VENDORS` option to conveniently enable all implemented vendor drivers
  - Updated root `CMakeLists.txt` to only add vendor subdirectories when implementations exist
  - Added `alpacacore_ioptron` static library with proper include paths and platform linking
  - Improved installation rules to optionally install vendor libraries when enabled
  - Added CMake namespace alias `AlpacaCore::alpacacore` for consistent target naming
  - Enhanced vendor library detection with proper target existence checks
- **Testing Infrastructure**
  - Updated Catch2 integration to use `Catch2::Catch2WithMain` and modern `catch_all.hpp` headers
  - Switched to explicit inclusion of Catch2 CMake module from `Catch2_DIR`
  - Updated test files for consistency
- **Repository Hygiene**
  - Extended `.gitignore` to ignore vendor SDK binaries and sources while keeping Markdown documentation under `external/`
  - Added cursor rules directory to `.gitignore`

