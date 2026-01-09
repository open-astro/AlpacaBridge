# Changelog

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

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

