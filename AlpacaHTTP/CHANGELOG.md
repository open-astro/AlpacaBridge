# Changelog

All notable changes to AlpacaHTTP will be documented in this file.

## [0.1.0] - 2025-12-02

### Added
- Initial release
- HTTP/1.1 server implementation
- Alpaca API routing
- JSON request/response handling
- Alpaca Discovery support
- Configuration management
- Error mapping to Alpaca specification
- Transaction ID management
- Logging integration with AlpacaCore

## [0.2.0] - 2025-01-XX

### Added
- **AlpacaCore Integration**: Full integration with AlpacaCore device registry
  - Router now uses AlpacaCore DeviceRegistry to retrieve devices
  - Device method dispatch for common AlpacaDriver methods (name, description, connected, etc.)
  - Support for device-specific method calls via HTTP API
- **Management Driver Support**: Server can now use AlpacaCore ManagementDriver
  - `/management/v1/description` endpoint uses ManagementDriver for server info
  - `/management/v1/configureddevices` lists all registered devices from registry
- **Enhanced Logging**: Complete integration with AlpacaCore logging system
  - Removed custom logging sink in favor of AlpacaCore's logging
  - All HTTP events now use AlpacaCore log system

### Changed
- **CMake Integration**: Updated CMakeLists.txt to link against AlpacaCore
  - Supports both installed AlpacaCore and workspace builds
  - Automatic detection of AlpacaCore in parent directory
- **Router Architecture**: Refactored to use AlpacaCore interfaces
  - Replaced placeholder DeviceManager with AlpacaCore DeviceRegistry
  - Implemented device method dispatch for GET/PUT requests
  - Added device type string-to-enum conversion
- **Server API**: Added `set_management_driver()` method to Server class
- **Example Server**: Updated to show AlpacaCore integration patterns

### Technical Details
- Device method dispatch supports:
  - Common properties: name, description, driverinfo, driverversion, interfaceversion
  - Connection management: connected (GET/PUT)
  - Actions: supportedactions, action, commandblind, commandbool, commandstring
- Device-specific methods (e.g., camera startexposure) can be extended in router dispatch

## [0.2.1] - 2025-12-04

### Changed
- **License Headers**: Updated all source files with new license header format
  - Updated copyright to include "and contributors"
  - Changed license URL to GitHub repository location
  - Added SSPL v1 compliance notice to all headers
  - Updated license reference from MongoDB URL to `https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE`

## [0.3.0] - 2025-12-16

### Added
- **Web UI**: Complete web-based device management interface
  - Modern HTML/CSS/JavaScript interface for device management
  - Device listing and status display
  - Device configuration and removal interface
  - Server information display
  - Accessible at root path (`/`) and `/web/` routes
  - Static file serving for web assets (HTML, CSS, JavaScript)
- **Telescope Driver Support**: Comprehensive telescope method dispatch
  - Full implementation of ASCOM Alpaca Telescope API methods
  - Support for 50+ telescope properties and methods including:
    - Position queries: rightascension, declination, altitude, azimuth, siderealtime
    - Motion control: slew, sync, park, unpark, findhome
    - Tracking control: tracking, trackingrate, trackingrates, declinationrate, rightascensionrate
    - Site information: sitelatitude, sitelongitude, siteelevation, utcdate
    - Capability queries: canpark, canunpark, canslew, cansync, canpulseguide, etc.
    - Guide rates: guideraterightascension, guideratedeclination
    - Target coordinates: targetrightascension, targetdeclination
    - Telescope properties: focallength, aperturediameter, aperturearea, sideofpier
  - Support for both GET (read) and PUT (write) operations
  - Parameter parsing from both query parameters and JSON request bodies
- **Management API Enhancements**:
  - `/management/v1/apiversions` endpoint for API version discovery
  - `/management/v1/configuredevice` endpoint for device configuration
  - `/management/v1/removedevice` endpoint for device removal
  - `/management/v1/shutdown` endpoint for graceful server shutdown
- **Device Setup Pages**: Support for device-specific setup endpoints
  - `/setup/v1/{devicetype}/{devicenumber}/setup` endpoint
  - Serves setup pages for device configuration
- **Shutdown Callback Support**: Graceful server shutdown mechanism
  - `set_shutdown_callback()` method on both Router and Server classes
  - Allows server to cleanly shut down when shutdown endpoint is called
  - Asynchronous callback execution after response is sent
- **Enhanced Request Handling**:
  - Root path (`/`) now serves web UI
  - Improved error handling for invalid endpoints
  - Better transaction ID management across all endpoints

### Changed
- **Router Architecture**: Major expansion of router capabilities
  - Added `dispatch_telescope_method()` for telescope-specific method routing
  - Enhanced device method dispatch with type-specific handlers
  - Improved parameter parsing with support for query params and JSON bodies
  - Better error responses with proper Alpaca error codes
- **Server Implementation**:
  - Added `server_fd_` atomic variable for better connection tracking
  - Enhanced shutdown handling with callback support
  - Improved async server lifecycle management
- **Example Server**:
  - Updated to use new logging API (`alpacahttp::util::log_info()`)
  - Added shutdown callback configuration
  - Improved shutdown sequence with proper thread cleanup
  - Better error handling and process termination
- **Request/Response Handling**:
  - Enhanced request parsing for both query parameters and JSON bodies
  - Improved response generation with proper content types
  - Better error mapping to Alpaca specification

### Technical Details
- Telescope method dispatch supports comprehensive ASCOM Alpaca Telescope API
- Parameter parsing helpers support double, int, bool, and string types
- Static file serving uses filesystem library for web asset delivery
- Shutdown callback executes asynchronously to ensure response is sent first
- Web UI uses modern JavaScript with fetch API for device management

