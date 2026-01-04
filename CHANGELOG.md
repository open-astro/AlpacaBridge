# Changelog

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

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

