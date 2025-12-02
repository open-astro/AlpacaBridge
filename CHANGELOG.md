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

