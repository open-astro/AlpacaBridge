# Architecture Guide

This guide provides an overview of AlpacaCore's architecture, design principles, and how components interact.

## Overview

AlpacaCore is a **vendor-neutral C++ core library** that implements ASCOM Alpaca device models and behavior. It is a **protocol-level library** that:

- Implements Alpaca device behavior/state
- Contains **no HTTP / REST / sockets**
- Avoids platform-specific hacks
- Exposes a clean API that higher-level servers use
- Follows the [ASCOM Alpaca API specification](https://ascom-standards.org/api/)

## Design Principles

### Protocol-Level Only

AlpacaCore is **protocol-level only**:

- Implements Alpaca device behavior/state
- No HTTP, REST, sockets, or JSON parsing
- Higher layers (e.g., AlpacaHTTP) translate HTTP/JSON into AlpacaCore calls

### Vendor Neutrality

- Core code contains **no vendor dependencies**
- Vendor SDKs are isolated in `external/` and `src/vendors/`
- Standard library only in core code
- Vendor-specific code is behind build options

### Clean Architecture

- Three-layer architecture for drivers (interface → wrapper → implementation)
- SDK complexity isolated in wrapper layer
- Platform-specific code in CMake, not C++ source
- Thread-safe design with no global mutable state

## Directory Structure

```
AlpacaCore/
├─ CMakeLists.txt
├─ LICENSE
├─ README.md
├─ CHANGELOG.md
├─ VERSION
├─ CONTRIBUTING.md
│
├─ external/                         # Vendor SDKs ONLY
│   ├─ ASICamera2_SDK/              # Vendor SDK folders (not in Git)
│   ├─ qhy_sdk/
│   └─ iOptron_SDK/
│
├─ include/
│   └─ alpacacore/                   # Official AlpacaCore SDK
│       ├─ alpaca_defs.h
│       ├─ alpaca_errors.h
│       ├─ alpaca_json.h
│       ├─ alpacadriver.h            # Base driver interface
│       ├─ managementdriver.h
│       ├─ device_capabilities.h
│       ├─ camera_driver.h           # Device driver interfaces
│       ├─ telescope_driver.h
│       ├─ filterwheel_driver.h
│       ├─ focuser_driver.h
│       ├─ rotator_driver.h
│       ├─ dome_driver.h
│       ├─ shutter_driver.h
│       ├─ switch_driver.h
│       ├─ util/
│       │   ├─ logging.h
│       │   ├─ threading.h
│       │   ├─ error_handling.h
│       │   └─ units.h
│       └─ vendor/                  # Vendor wrapper APIs
│           ├─ zwo/
│           ├─ qhy/
│           └─ ioptron/
│
├─ src/
│   ├─ core/
│   │   ├─ alpacadriver.cpp
│   │   ├─ alpaca_defs.cpp
│   │   ├─ alpaca_errors.cpp
│   │   ├─ managementdriver.cpp
│   │   └─ util/
│   │       ├─ logging.cpp
│   │       ├─ threading.cpp
│   │       ├─ error_handling.cpp
│   │       └─ units.cpp
│   │
│   ├─ drivers/
│   │   ├─ camera_driver.cpp         # Base implementations (optional)
│   │   ├─ telescope_driver.cpp
│   │   └─ ...
│   │
│   ├─ vendors/
│   │   ├─ zwo/
│   │   │   ├─ zwo_sdk_wrapper.cpp
│   │   │   ├─ zwo_camera_driver.cpp
│   │   │   └─ CMakeLists.txt
│   │   └─ qhy/
│   │       └─ ...
│   │
│   └─ management/
│       ├─ discovery_service.cpp
│       └─ device_registry.cpp
│
├─ tests/
│   ├─ CMakeLists.txt
│   ├─ test_alpaca_defs.cpp
│   ├─ test_device_registry.cpp
│   ├─ test_error_handling.cpp
│   └─ ...
│
└─ examples/
    ├─ minimal_camera/
    ├─ minimal_telescope/
    └─ simulate_all_devices/
```

## Three-Layer Driver Architecture

AlpacaCore drivers follow a strict three-layer architecture:

### Layer 1: Pure Virtual Interface

**Location**: `include/alpacacore/<device>_driver.h`

- Public API that all drivers implement
- Inherits from `AlpacaDriver` base class
- Defines the contract that all drivers must follow
- No vendor dependencies

### Layer 2: SDK Wrapper

**Location**: `include/alpacacore/vendor/<vendor>/<vendor>_sdk_wrapper.h`

- Clean C++ interface wrapping messy vendor SDKs
- Isolates SDK dependencies from core code
- Provides clean, standard C++ types
- Only place where SDK headers are included (in implementation)

### Layer 3: Vendor Implementation

**Location**: `src/vendors/<vendor>/<vendor>_<device>_driver.cpp`

- Concrete driver implementation
- Uses wrapper layer, never touches raw SDK directly
- Implements all pure virtual methods from Layer 1

See [Driver Development Guide](driver-development.md) for detailed information.

## Component Overview

### Core Components

#### AlpacaDriver

Base interface for all Alpaca device drivers. Provides:
- Device information (name, description, unique ID)
- Connection state management
- Common Alpaca device API

#### Device Drivers

Pure virtual interfaces for each device type:
- `CameraDriver` - Camera operations (exposure, image capture)
- `TelescopeDriver` - Telescope operations (slewing, tracking)
- `FilterWheelDriver` - Filter wheel operations
- `FocuserDriver` - Focuser operations
- And more...

#### Utilities

- **Logging**: Minimal, dependency-free logging system
- **Threading**: Thread-safe primitives and utilities
- **Error Handling**: Unified exception handling (`AlpacaException`)
- **Units**: Unit conversion utilities

### Management Components

#### Device Registry

Manages registered device instances and provides discovery.

#### Discovery Service

Handles device discovery and enumeration.

## Threading Model

- **No global mutable state** - All state is instance-local
- **Device instances manage their own locking** - Each driver instance is responsible for thread safety
- **Higher layers manage concurrency** - AlpacaCore provides thread-safe primitives, but concurrency is managed by higher layers

## Error Handling

- **Single exception type**: `AlpacaException` (derived from `std::runtime_error`)
- **No error-code return values** - Use exceptions for error handling
- **Higher layers map exceptions** - AlpacaHTTP maps exceptions → Alpaca error numbers

## Build System

### CMake Options

- `ALPACACORE_BUILD_TESTS`: Build unit tests (default: ON)
- `ALPACACORE_ENABLE_ZWO`: Enable ZWO vendor support (default: OFF)
- `ALPACACORE_ENABLE_QHY`: Enable QHY vendor support (default: OFF)
- `ALPACACORE_ENABLE_IOPTRON`: Enable iOptron vendor support (default: OFF)

### Platform Support

- **Linux**: Primary target, full support
- **macOS**: Full support
- **Windows**: Full support

Platform-specific code is handled in CMake, not in C++ source.

## Testing Architecture

- **Catch2** framework for unit tests
- Tests organized by component and vendor
- Hardware tests tagged with `[hardware]` for CI filtering
- Mock support for testing without hardware

See [Testing Guide](testing.md) for detailed information.

## API Design

### Synchronous C++ Methods

AlpacaCore provides synchronous C++ methods. Higher layers handle async operations.

### DTO Structs

Response structures follow Alpaca patterns:

```cpp
struct AlpacaResponseHeader {
    int client_transaction_id{};
    int server_transaction_id{};
    int error_number{};
    std::string error_message;
};
```

### No Hidden Global State

All state is explicit and instance-based. No singletons or global state.

## Units & Conventions

- **Exposure**: seconds
- **Angles**: degrees
- **RA**: hours
- **Dec**: degrees
- **Pixel size**: microns
- **Time**: UTC, `std::chrono`
- **Wavelengths**: nm

## Integration with Higher Layers

AlpacaCore is designed to be used by higher-level servers:

```
┌─────────────────┐
│   AlpacaHTTP     │  ← HTTP/REST server
│  (AlpacaOS)     │
└────────┬────────┘
         │
         │ Translates HTTP/JSON
         │ to AlpacaCore calls
         ▼
┌─────────────────┐
│   AlpacaCore    │  ← Protocol-level library
│  (This Project) │
└────────┬────────┘
         │
         │ Uses vendor SDKs
         ▼
┌─────────────────┐
│  Vendor SDKs    │
│  (external/)    │
└─────────────────┘
```

## Best Practices

1. **Keep core vendor-neutral** - No vendor dependencies in core
2. **Isolate SDK complexity** - Use wrapper layer
3. **Platform code in CMake** - Not in C++ source
4. **Thread-safe by design** - No global mutable state
5. **Comprehensive testing** - Unit tests for all functionality
6. **Clear documentation** - Doxygen comments for public API

## Future Considerations

- API documentation generation (Doxygen)
- Additional device types as needed
- Performance optimizations
- Extended platform support

## Additional Resources

- [Driver Development Guide](driver-development.md) - Building vendor drivers
- [Testing Guide](testing.md) - Testing architecture and practices
- [ASCOM Alpaca API Specification](https://ascom-standards.org/api/)
- [README.md](../../README.md) - Project overview
