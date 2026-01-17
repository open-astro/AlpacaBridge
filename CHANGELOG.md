# Changelog

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

## [0.8.4] - 2026-01-17

### Added
- **ImageBytes Streaming** (AlpacaHTTP)
  - Added `application/imagebytes` handling so `camera.imagearray` can stream compact binary payloads instead of JSON when clients request it.
  - Honored `camera.imagearrayvariant` metadata, inferred transmission element widths, and included numeric metadata plus transaction IDs alongside the pixel data.
  - Streamed structured error payloads with the same metadata layout so Alpaca exceptions can still be parsed when image bytes responses fail.

## [0.8.3] - 2026-01-13

### Changed
- **Web UI Enhancements** (AlpacaHTTP)
  - Added collapsible device cards with expand/collapse toggle buttons
  - Improved device list organization with consistent device type ordering (telescope, camera, filterwheel, focuser, rotator, dome, switch)
  - Enhanced device sorting by type, device number, and name
  - Improved filter wheel configuration UI with slot count selector (5, 7, 8 slots, or custom)
  - Added filter wheel preset options with common filter names (Luminance, RGB, Ha, OIII, SII, Sloan filters, Clear, Dark, UV, IR)
  - Filter wheel preset lookup with alias support for flexible filter name matching
  - Filter wheel slot management with individual slot configuration rows
  - Advanced filter name editing moved to collapsible details section
  - Better visual organization of device information and settings
  - Enhanced CSS styling for device cards, toggles, and filter wheel controls

## [0.8.2] - 2026-01-12

### Added
- **Windows 11 x64 Platform Support** (AlpacaCore)
  - Added Windows 11 x64 verification and testing documentation
  - Windows support verified for all ZWO drivers (ASI cameras, EFW filter wheel, EAF focuser, CAA rotator)
  - Windows support verified for iOptron telescope mounts
  - Added Windows library binaries for ZWO CAA rotator SDK
  - Windows driver requirement documentation for ZWO ASI cameras
  - Updated SUPPORTED-DRIVERS.md with Windows 11 x64 platform support across all driver tables
  - Added Windows Notes section to General Notes with USB and serial connection guidance
- **Socket Utilities** (AlpacaHTTP)
  - New `socket_utils.h` header for cross-platform socket operations

### Changed
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with Windows 11 x64 verification status for all drivers
  - Added Windows driver requirement note for ZWO ASI cameras (driver must be installed from ZWO)
  - Updated Verified OS/Arch entries to include Windows 11 (x64) for all tested drivers
  - Removed EAF Pro from focuser table (not available for testing)
  - Enhanced Windows Notes section with USB and serial connection information
- **Build System** (AlpacaCore)
  - Updated ZWO CMakeLists.txt to support Windows library binaries
- **Discovery Protocol** (AlpacaHTTP)
  - Enhanced discovery protocol implementation
- **Server Implementation** (AlpacaHTTP)
  - Improved server socket handling and utilities
- **iOptron Protocol** (AlpacaCore)
  - Enhanced iOptron protocol wrapper and telescope driver implementation
- **Logging** (AlpacaCore)
  - Improved logging implementation
- **Build Scripts** (AlpacaBridge)
  - Updated Windows build script (`build_and_run.cmd`)

## [0.8.1] - 2026-01-11

### Added
- **Linux Installation Script** (AlpacaBridge)
  - New `install_alpaca_service.sh` script for Linux (arm64) systems
  - Supports `install`, `update`, `uninstall`, and `status` commands
  - Automatically builds AlpacaCore and AlpacaHTTP, installs udev rules, and creates systemd service
  - Configurable via environment variables (ALPACAHTTP_USE_BOOST_BEAST, ALPACACORE_ENABLE_ALL_VENDORS, ALPACA_INSTALL_UDEV_RULES, ALPACA_GIT_PULL, ALPACA_CLEAN_BUILD)
  - Auto-detects CPU cores for parallel builds
- **Linux ARMv8 Platform Support** (AlpacaCore)
  - Added ARMv8 (arm64) Linux library binaries for all ZWO drivers (CAA, EAF, EFW)
  - Marked Linux ARMv8 as tested and verified for all ZWO and iOptron drivers
  - Updated SUPPORTED-DRIVERS.md with Linux ARMv8 verification status
  - Added udev rules for ZWO CAA rotator devices
- **Parallel Test Execution** (AlpacaBridge)
  - Enabled parallel test execution in `run_all_tests.sh`
  - Auto-detects CPU cores (sysctl on macOS, nproc on Linux)
  - Uses parallel cmake builds and ctest execution for faster test runs

### Changed
- **Build System** (AlpacaCore)
  - Added libudev dependency detection and linking for ZWO driver on Linux
  - Prefer pkg-config for libudev detection, fallback to find_library
  - Require libudev on Linux (non-Apple) platforms
- **Build Scripts** (AlpacaBridge)
  - Added automatic udev rules installation in `build_and_run.sh` for Linux
  - Configurable via `ALPACA_INSTALL_UDEV_RULES` environment variable (default: ON)
  - Automatically finds and installs all `.rules` files from `external/` directory
  - Reloads udev rules and triggers after installation
- **Test Infrastructure** (AlpacaCore)
  - Updated all test files to use `catch2_compat.h` instead of `catch2/catch_all.hpp`
  - Improved test compatibility and consistency across test suite
- **Documentation** (AlpacaBridge)
  - Added installation section to README.md with install script documentation
  - Updated AGENTS.md with note about udev rules installation requirement
  - Updated SUPPORTED-DRIVERS.md with Linux ARMv8 testing notes and verification status
- **Git Configuration** (AlpacaCore)
  - Added vendor SDK allowlist rules in `.gitignore` for ZWO SDK directories (CAA, EFW, EAF)
  - Allows vendor SDK files to be tracked in repository for easier distribution

## [0.8.0] - 2026-01-10

### Added
- **ZWO EFW Filter Wheel Driver** (AlpacaCore)
  - Complete ZWO EFW (Electronic Filter Wheel) driver implementation with full ASCOM Alpaca FilterWheel API support
  - SDK wrapper layer for ZWO EFW SDK Version 1.8.4
  - Support for USB connection via libusb-1.0
  - Comprehensive filter wheel property support (position, names, focus offsets, slot count)
  - Position control with slot count validation
  - Filter name and focus offset management
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Filter wheel binding via `filterwheelId` or `filterwheelIndex` configuration
  - ConformU validated for EFW on macOS (arm64) with 0 errors and 0 issues
- **FilterWheel Device Support** (AlpacaHTTP)
  - Complete FilterWheel device method routing and dispatch
  - Support for all ASCOM Alpaca FilterWheel API methods (position, names, focusoffsets)
  - ZWO EFW filter wheel device registration and configuration
  - Filter wheel device discovery and management via web UI
  - Smart auto-numbering for filter wheel device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO EFW filter wheel device registration with validation
  - Filter wheel device registration with filter wheel binding support
  - Improved device configuration validation for filter wheel devices
  - Smart filter wheel index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Filter wheel device type support in device configuration
  - ZWO filter wheel index and ID configuration fields
  - Filter names textarea input for custom filter naming
  - Auto-fill support for filter wheel indices
  - Enhanced vendor-specific configuration UI for ZWO filter wheels
- **Build System** (AlpacaBridge)
  - Updated `.gitignore` to allow `AlpacaCore/external/ZWO` folder and subfolders
  - ZWO SDK files (CAA, EAF, and EFW) now included in repository for easier distribution

## [0.7.0] - 2026-01-09

### Added
- **ZWO CAA Rotator Driver** (AlpacaCore)
  - Complete ZWO CAA (Camera Angle Adjuster) rotator driver implementation with full ASCOM Alpaca Rotator API support
  - SDK wrapper layer for ZWO CAA SDK Version 1.5.9
  - Support for USB connection via libusb-1.0
  - Comprehensive rotator property support (position, mechanical position, target position, step size, reverse, etc.)
  - Absolute position control with mechanical position support
  - Rotator movement control (move absolute, move, move mechanical, halt)
  - Position synchronization with sync offset support
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Rotator binding via `rotatorId` or `rotatorIndex` configuration
  - ConformU validated for CAA on macOS (arm64) with 0 errors and 0 issues
- **Rotator Device Support** (AlpacaHTTP)
  - Complete Rotator device method routing and dispatch
  - Support for all ASCOM Alpaca Rotator API methods
  - ZWO CAA rotator device registration and configuration
  - Rotator device discovery and management via web UI
  - Smart auto-numbering for rotator device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO CAA rotator device registration with validation
  - Rotator device registration with rotator binding support
  - Improved device configuration validation for rotator devices
  - Smart rotator index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Rotator device type support in device configuration
  - ZWO rotator index and ID configuration fields
  - Auto-fill support for rotator indices
  - Enhanced vendor-specific configuration UI for ZWO rotators
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ZWO CAA rotator information
  - Reordered all driver sections to match ASCOM API device type order (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope)
  - Added placeholders for all ASCOM device types not yet implemented
  - Documented CAA SDK version and platform support
  - Added Linux USB permissions documentation for CAA devices

## [0.6.1] - 2026-01-08

### Added
- **ZWO Camera Support** (AlpacaCore)
  - Added ASI120MM Mini camera to supported devices list
  - ConformU validated for ASI120MM Mini on macOS (arm64)

### Changed
- **ZWO Camera Driver** (AlpacaCore)
  - Improved camera info preloading for faster device name resolution
  - Added camera info refresh mechanism to ensure accurate device information
  - Enhanced camera enumeration and info caching for better performance
- **iOptron Telescope Driver** (AlpacaCore)
  - Updated driver description to accurately reflect supported mount series
  - Improved description clarity for all supported iOptron mount models
- **Web UI** (AlpacaHTTP)
  - Added cache busting for device list loading to prevent stale data
  - Improved device list refresh reliability with timestamp-based cache control
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ASI120MM Mini camera
  - Enhanced building guide with workspace-level script documentation
  - Improved documentation for ZWO driver support (camera, switch, focuser)

## [0.6.0] - 2026-01-06

### Added
- **ZWO EAF Focuser Driver** (AlpacaCore)
  - Complete ZWO EAF (Electronic Auto Focuser) driver implementation with full ASCOM Alpaca Focuser API support
  - SDK wrapper layer for ZWO EAF Focuser SDK Version 1.7.7
  - Support for USB connection via libusb-1.0
  - Comprehensive focuser property support (position, max step, temperature, etc.)
  - Absolute position control with step range support
  - Focuser movement control (move, stop, is moving detection)
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Focuser binding via `focuserId` or `focuserIndex` configuration
  - Support for EAF and EAF Pro models
  - ConformU validated for EAF and EAF Pro on macOS (arm64)
  - Note: EAF Pro Bluetooth version currently only works with USB connection (Bluetooth support not yet implemented)
- **Focuser Device Support** (AlpacaHTTP)
  - Complete Focuser device method routing and dispatch
  - Support for all ASCOM Alpaca Focuser API methods
  - ZWO EAF focuser device registration and configuration
  - Focuser device discovery and management via web UI
  - Smart auto-numbering for focuser device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO EAF focuser device registration with validation
  - Focuser device registration with focuser binding support
  - Improved device configuration validation for focuser devices
  - Smart focuser index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Focuser device type support in device configuration
  - ZWO focuser index and ID configuration fields
  - Auto-fill support for focuser indices
  - Enhanced vendor-specific configuration UI for ZWO focusers
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ZWO EAF focuser information
  - Added Focuser Drivers section to supported drivers documentation
  - Documented EAF SDK version and platform support
  - Added note about EAF Pro Bluetooth limitation (USB only)

## [0.5.0] - 2026-01-05

### Added
- **ZWO Camera Driver** (AlpacaCore)
  - Complete ZWO ASI camera driver implementation with full ASCOM Alpaca Camera API support
  - SDK wrapper layer for ZWO ASI Camera SDK Version 1.40
  - Support for USB connection via libusb-1.0
  - Comprehensive camera property support (binning, ROI, gain, offset, temperature, etc.)
  - Exposure control with light/dark frame support
  - Pulse guiding support for autoguiding
  - Image array retrieval with optimized payload building
  - Asynchronous pulse guide implementation
  - Device state telemetry and connection management
  - ConformU validated for 6 camera models:
    - ASI174MM Mini
    - ASI290MM Mini
    - ASI462MM
    - ASI662MC
    - ASI2600MC Pro
    - ASI2600MM Pro
- **ZWO Switch Driver** (AlpacaCore)
  - Dew heater switch device implementation for ZWO cameras with anti-dew heater support
  - Automatic detection of cameras with `ASI_ANTI_DEW_HEATER` SDK control
  - Camera binding via `cameraId` or `cameraIndex` configuration
  - Full ASCOM Alpaca Switch API implementation (ISwitchV3)
  - Asynchronous switch state change support
  - ConformU validated for dew heater on:
    - ASI2600MC Pro
    - ASI2600MM Pro
- **Switch Device Support** (AlpacaHTTP)
  - Complete Switch device method routing and dispatch
  - Support for all ASCOM Alpaca Switch API methods
  - ZWO dew heater switch device registration and configuration
  - Switch device discovery and management via web UI
- **Server Restart Functionality** (AlpacaHTTP)
  - `/management/v1/restart` endpoint for graceful server restart
  - Restart callback support for custom restart handling
  - Thread-safe restart request handling with duplicate request prevention
  - Automatic server restart with connection cleanup and reinitialization
  - Restart button in web UI for easy server management
- **Smart Auto-Numbering** (AlpacaHTTP)
  - Automatic device number assignment based on existing devices
  - Smart camera index auto-fill for ZWO cameras
  - Automatic detection of next available device number per device type
  - User-modified field detection to preserve manual entries
  - Real-time auto-fill as device type changes in web UI
- **Transaction ID Management** (AlpacaHTTP)
  - Automatic server transaction ID generation using atomic counter
  - Thread-safe transaction ID assignment for concurrent requests
  - Client transaction ID parsing from query parameters, JSON bodies, and form data
  - Case-insensitive transaction ID extraction from form data
  - Proper transaction ID propagation in all Alpaca responses
- **Image Array Optimization** (AlpacaHTTP)
  - Optimized image array JSON payload building with pre-allocated buffers
  - Efficient integer-to-string conversion for large image arrays
  - Support for 2D and 3D image arrays (monochrome and color)
  - Improved performance for image transfer over HTTP
  - Direct string building to avoid JSON library overhead for large arrays
- **ConformU Validation Infrastructure** (AlpacaCore)
  - Comprehensive ConformU test results for all verified drivers
  - Test result organization by vendor and device type
  - Documentation of validated devices in SUPPORTED-DRIVERS.md
  - ConformU validation date tracking for all certified devices
- **Workspace Infrastructure** (AlpacaBridge)
  - Added AGENTS.md file with instructions for AI agents and Cursor workflows
  - Centralized reference to Cursor rules files for AlpacaCore and AlpacaHTTP
  - Clear documentation of agent workflow requirements and rule locations

### Changed
- **Router Architecture** (AlpacaHTTP)
  - Enhanced error handling to maintain HTTP 200 status for Alpaca error responses
  - Added `cancelasync` method to Switch device method set
  - Improved switch device method routing and parameter handling
  - Better device registration error messages for ZWO devices
  - Improved transaction ID extraction from multiple request sources (query, JSON, form)
  - Enhanced parameter parsing for query parameters, JSON bodies, and form data
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO camera device registration with validation
  - ZWO switch device registration with camera binding support
  - Improved device configuration validation and error reporting
  - Smart device number and camera index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Smart auto-numbering for device numbers and camera indices
  - Automatic next available number detection per device type
  - ZWO switch type selection (dew heater) in device configuration
  - Server restart button in server management interface
  - Improved device configuration form with auto-fill capabilities
  - Better handling of device editing vs. new device creation
  - Enhanced vendor-specific configuration UI for ZWO devices
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with all ConformU-validated ZWO cameras and switches
  - Added Switch Drivers section to supported drivers documentation
  - Updated ConformU README with ZWO test results
  - Documented dew heater switch functionality and camera binding

## [0.4.0] - 2026-01-03

### Added
- **Logging System Enhancements** (AlpacaCore & AlpacaHTTP)
  - Log level filtering with configurable minimum log level
  - Log history capture with configurable history limit
  - External log sink support for custom logging integrations
  - `/management/v1/logs` endpoint for retrieving log history
  - `/management/v1/loglevel` endpoint for dynamic log level control
  - Log level applied at initialization from configuration
- **Web UI Enhancements** (AlpacaHTTP)
  - Interactive log level toggles with real-time updates
  - Log history display in web interface
  - Logo and improved styling throughout the UI
  - Device settings interface with configuration controls
  - Enhanced UI cleanup and user experience improvements
  - Improved server info display with formatted grid layout
  - Editable server location field with inline save functionality
  - Better JSON response parsing and error handling in UI
- **Device Persistence** (AlpacaHTTP)
  - Automatic device configuration persistence to `config/registered_devices.json`
  - Device registration persists across server restarts
  - Device removal and configuration management via API
- **Thread Pool Support** (AlpacaHTTP)
  - Configurable thread pool for concurrent request handling
  - Default pool size of 32 threads (configurable via environment variable)
  - Improved performance for multiple concurrent clients
- **Error Code Support** (AlpacaCore)
  - Error code support in `AlpacaException` for proper ASCOM error mapping
  - Improved error code mapping from AlpacaCore to HTTP responses
  - Better error reporting and debugging capabilities
- **Discovery Protocol Improvements** (AlpacaHTTP)
  - JSON response format for Alpaca Discovery protocol
  - Enhanced discovery compatibility with ASCOM clients
- **Server Location Management** (AlpacaHTTP)
  - PUT/POST support for `/management/v1/description` endpoint to update server location
  - Automatic persistence of location changes to YAML configuration file
  - Thread-safe server information management with configurable server name, manufacturer, and version
- **Version Information** (AlpacaHTTP)
  - Version header file (`version.h`) for compile-time version access
  - Centralized version management in CMake build system
- **Workspace Infrastructure** (AlpacaBridge)
  - Consolidated CHANGELOG.md at workspace level
  - Comprehensive README.md with logo, quick start guide, and build instructions
  - Cross-platform build and test scripts (`build_and_run.sh/cmd`, `run_all_tests.sh/cmd`)
  - Workspace-level `.gitignore` for build artifacts and local configuration
  - Workspace-level LICENSE file

### Changed
- **Router Architecture** (AlpacaHTTP)
  - Enhanced parameter parsing for query parameters and JSON request bodies
  - Auto-parse JSON strings in response values for better compatibility
  - Improved form parsing for device configuration
  - Better error responses with proper Alpaca error codes
  - Case-insensitive query parameter lookup support
  - YAML configuration file editing for server location persistence
  - Improved error status application with proper HTTP status code mapping
- **iOptron Telescope Driver** (AlpacaCore)
  - Improved status parsing and command handling
  - Enhanced slew completion detection
  - Better protocol compliance and error handling
  - ConformU validation and certification updates
- **Build System** (AlpacaCore & AlpacaHTTP)
  - Added `clean-all` target for complete build cleanup
  - Vendor compile definitions for better SDK integration
  - Fixed circular dependency warnings in CMake
- **Documentation** (AlpacaCore & AlpacaHTTP)
  - Updated cursor rules for development workflow
  - Enhanced external documentation
  - Added full SSPL v1 license text to repository

## [0.3.0] - 2025-12-16

### Added
- **iOptron Telescope Driver** (AlpacaCore 0.3.0)
  - Complete telescope driver implementation for iOptron mounts
  - Protocol wrapper for RS-232 command set over serial or TCP
  - Support for position, motion, tracking, and site information
- **Web UI** (AlpacaHTTP 0.3.0)
  - Modern web-based device management interface
  - Device listing, configuration, and status display
  - Accessible at root path (`/`) and `/web/` routes
- **Telescope Driver Support** (AlpacaHTTP 0.3.0)
  - Comprehensive telescope method dispatch (50+ methods)
  - Full ASCOM Alpaca Telescope API implementation
- **Management API Enhancements** (AlpacaHTTP 0.3.0)
  - API version discovery endpoint
  - Device configuration and removal endpoints
  - Graceful server shutdown support
- **Comprehensive Developer Documentation** (AlpacaCore 0.3.0)
  - Complete building guide with prerequisites and troubleshooting
  - Driver development guide with three-layer architecture
  - Testing guide with comprehensive coverage
  - Architecture documentation

### Changed
- **Build System** (AlpacaCore 0.3.0)
  - Added `ALPACACORE_ENABLE_ALL_VENDORS` option
  - Enhanced vendor library detection and installation
  - Improved CMake namespace aliasing
- **Router Architecture** (AlpacaHTTP 0.3.0)
  - Major expansion with telescope-specific method routing
  - Enhanced parameter parsing for query params and JSON bodies
  - Improved error responses with proper Alpaca error codes
- **Testing Infrastructure** (AlpacaCore 0.3.0)
  - Updated to modern Catch2 integration
  - Enhanced test file consistency

## [0.2.1] - 2025-12-04

### Changed
- **License Headers** (AlpacaHTTP 0.2.1)
  - Updated all source files with new license header format
  - Changed license URL to GitHub repository location
  - Added SSPL v1 compliance notice to all headers

## [0.2.0] - 2025-12-02

### Added
- **DeviceRegistry** (AlpacaCore 0.2.0)
  - Complete device management system with singleton pattern
  - Thread-safe device registration and lookup
  - Device capability enumeration for management API
- **AlpacaCore Integration** (AlpacaHTTP 0.2.0)
  - Full integration with AlpacaCore device registry
  - Device method dispatch for common AlpacaDriver methods
  - Management Driver support for server information

### Changed
- **License URLs** (AlpacaCore 0.2.0)
  - Updated license URL in all source files to GitHub repository location
  - Updated 49 files (headers, sources, and tests)
- **CMake Integration** (AlpacaHTTP 0.2.0)
  - Updated to link against AlpacaCore
  - Supports both installed AlpacaCore and workspace builds
- **Router Architecture** (AlpacaHTTP 0.2.0)
  - Refactored to use AlpacaCore interfaces
  - Replaced placeholder DeviceManager with AlpacaCore DeviceRegistry

## [0.1.0] - 2025-12-02

### Added
- **Initial Release**
  - AlpacaCore: Core Alpaca protocol library with device driver interfaces
  - AlpacaHTTP: HTTP/1.1 server with Alpaca API routing
  - Complete directory structure following architecture guidelines
  - CMake build system with C++20 support
  - Test infrastructure with Catch2 support
  - Example servers and device implementations
  - Comprehensive documentation

