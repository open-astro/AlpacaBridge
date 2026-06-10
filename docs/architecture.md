# Architecture

AlpacaBridge is a complete ASCOM Alpaca server for Linux, built from two main components:

- **AlpacaCore** — vendor-neutral C++ driver library implementing ASCOM Alpaca device models
- **AlpacaHTTP** — HTTP server that translates REST/JSON requests into AlpacaCore calls and serves the web management UI

## System layers

```
+---------------------------+
|        Web Browser        |  <- Management UI (port 6800)
+------------+--------------+
             |
             | HTTP/JSON
             v
+---------------------------+
|        AlpacaHTTP         |  <- HTTP server, router, config,
|  (router, web UI, config) |     Alpaca discovery
+------------+--------------+
             |
             | C++ method calls
             v
+---------------------------+
|        AlpacaCore         |  <- Device driver library
|  (drivers, wrappers,      |     (no HTTP, no sockets)
|   device registry)        |
+------------+--------------+
             |
             | Vendor SDK / serial / network
             v
+---------------------------+
|    Vendor SDKs & Hardware |
|    (external/)            |
+---------------------------+
```

## AlpacaCore

AlpacaCore is a **protocol-level library** that:

- Implements Alpaca device behavior and state machines
- Contains **no HTTP, REST, sockets, or JSON parsing**
- Exposes a clean C++ API that higher-level servers use
- Follows the [ASCOM Alpaca API specification](https://ascom-standards.org/api/)

### Design principles

- **Vendor neutrality** — core code contains no vendor dependencies; vendor SDKs are isolated in `external/` and `src/vendors/`; vendor-specific code is behind CMake build options
- **Three-layer driver architecture** — interface -> wrapper -> implementation (see below)
- **Standard library only in core** — no vendor headers leak into public API
- **Platform-specific code in CMake** — not in C++ source
- **Thread-safe, no global mutable state** — all state is instance-local

### Device driver interfaces

AlpacaCore defines pure virtual interfaces for all ASCOM device types:

| Interface | Header |
|-----------|--------|
| Camera | `camera_driver.h` |
| CoverCalibrator | `covercalibrator_driver.h` |
| Dome | `dome_driver.h` |
| FilterWheel | `filterwheel_driver.h` |
| Focuser | `focuser_driver.h` |
| ObservingConditions | `observingconditions_driver.h` |
| Rotator | `rotator_driver.h` |
| SafetyMonitor | `safetymonitor_driver.h` |
| Switch | `switch_driver.h` |
| Telescope | `telescope_driver.h` |

All interfaces inherit from `AlpacaDriver` which provides device information, connection state, and the common Alpaca device API.

### Three-layer driver architecture

#### Layer 1: Pure virtual interface

**Location**: `include/alpacacore/<device>_driver.h`

- Public API that all drivers implement
- Inherits from `AlpacaDriver` base class
- No vendor dependencies

#### Layer 2: SDK/Protocol wrapper

**Location**: `include/alpacacore/vendor/<vendor>/<vendor>_sdk_wrapper.h` or `<vendor>_protocol_wrapper.h`

- Clean C++ interface wrapping vendor SDKs or serial/network protocols
- Isolates SDK dependencies from core code
- Only place where vendor SDK headers are included

Drivers that talk to hardware via a vendor C library use an **SDK wrapper** (ZWO, QHY, Player One, SVBONY, ToupTek). Drivers that talk over serial/network protocols use a **protocol wrapper** (iOptron, SynScan, Celestron, Losmandy Gemini).

#### Layer 3: Vendor implementation

**Location**: `src/vendors/<vendor>/<vendor>_<device>_driver.cpp`

- Concrete driver implementation
- Uses wrapper layer, never touches raw SDK directly
- Implements all pure virtual methods from Layer 1

See the [Development Guide](development.md) for step-by-step implementation.

### Vendor drivers

| Vendor | Device Types | Wrapper Type | Status |
|--------|-------------|--------------|--------|
| ZWO | Camera, Focuser (EAF), Rotator (CAA), Switch (dew heater), FilterWheel (EFW) | SDK wrapper | Production |
| QHY | Camera | SDK wrapper | Production |
| Player One | Camera | SDK wrapper | Production |
| SVBONY | Camera | SDK wrapper | Production |
| ToupTek | Camera | SDK wrapper | Production |
| iOptron | Telescope | Protocol wrapper | Production |
| SynScan | Telescope | Protocol wrapper | Production |
| Celestron | Telescope | Protocol wrapper | Production |
| Losmandy Gemini | Focuser | Protocol wrapper | Production |
| WeeWX | ObservingConditions | HTTP client | Production |
| Bisque | Telescope | Script/protocol | In development |

### Management components

- **Device Registry** — manages registered device instances and provides enumeration
- **Discovery Service** — handles Alpaca UDP discovery protocol

### Gold-standard driver behavior

Every driver should follow these runtime semantics:

- **Async connect/disconnect**: `connect()` and `disconnect()` return immediately and perform work in a background task
- **Connecting state**: `get_connecting()` reports true while connect/disconnect is in progress
- **Device state snapshot**: `get_device_state()` returns a populated list of Name/Value pairs representing key device telemetry
- **Connected property**: `set_connected()` stays synchronous to support legacy clients

Use the iOptron telescope driver as the reference implementation.

## AlpacaHTTP

AlpacaHTTP is the HTTP server layer. It translates Alpaca REST/JSON requests into AlpacaCore C++ method calls and serves the web management UI.

### Components

- **Server** (`src/http/server.cpp`) — POSIX socket server, thread-per-connection model, listens on port 6800
- **Router** (`src/http/router.cpp`) — maps HTTP routes to AlpacaCore driver method calls for all device types
- **Config** (`src/core/config.cpp`) — JSON-based device configuration, persisted to disk
- **Discovery** (`src/discovery/discovery.cpp`) — Alpaca UDP discovery responder (port 32227)
- **Web UI** (`web/`) — browser-based management interface for device configuration
- **Error Mapping** (`src/util/error_mapping.cpp`) — maps `AlpacaException` to Alpaca error numbers
- **JSON Utils** (`src/core/json_utils.cpp`) — request/response serialization

### Web management UI

The web UI (`web/index.html`, `web/app.js`, `web/style.css`) provides:

- Device listing and status
- Add/remove/configure devices
- Vendor-specific configuration (connection type, port path, camera index, etc.)
- Server info and shutdown/restart controls

Served as static files from the `web/` directory at `http://localhost:6800/management/`.

## Workspace structure

```
AlpacaBridge/
+- AlpacaCore/                        # Driver library
|   +- include/alpacacore/            # Public headers
|   |   +- *_driver.h                 # Device driver interfaces
|   |   +- vendor/                    # Vendor wrapper headers
|   |       +- zwo/ qhy/ ioptron/ ... # One directory per vendor
|   +- src/
|   |   +- core/                      # Base implementations, utilities
|   |   +- drivers/                   # Base driver implementations
|   |   +- vendors/                   # Vendor-specific implementations
|   |   |   +- zwo/ qhy/ ioptron/    # One directory per vendor
|   |   |   +- synscan/ celestron/
|   |   |   +- playerone/ svbony/ touptek/
|   |   |   +- gemini/ weewx/ bisque/
|   |   +- management/               # Device registry, discovery
|   +- external/                      # Vendor SDKs and protocol docs
|   |   +- ZWO/ QHY/ PlayerOne/      # SDK libraries
|   |   +- SVBONY/ ToupTek/
|   |   +- iOptron/ SynScan/         # Protocol documentation
|   |   +- Celestron/ Losmandy/
|   |   +- Bisque/                   # In development
|   +- tests/                        # Catch2 unit tests
|   +- conformu/                     # ConformU validation reports
|       +- ZWO/ QHY/ iOptron/ ...    # Per-vendor, per-model results
|
+- AlpacaHTTP/                        # HTTP server
|   +- include/alpacahttp/           # Public headers
|   +- src/
|   |   +- core/                     # Config, request/response, JSON
|   |   +- http/                     # Server, router
|   |   +- discovery/                # Alpaca UDP discovery
|   |   +- util/                     # Logging, error mapping
|   +- web/                          # Management UI (HTML/JS/CSS)
|   +- tests/                        # HTTP routing/config tests
|
+- debian/                            # Debian packaging
+- docs/                              # Documentation
+- AGENTS.md                          # AI driver development guide
+- SUPPORTED-DRIVERS.md               # ConformU-validated driver matrix
+- CHANGELOG.md                       # Release notes
+- build_and_run.sh                   # Build and start server
+- run_all_tests.sh                   # Run all test suites
+- install_alpaca_service.sh          # Install as systemd service
```

## Threading model

- **No global mutable state** — all state is instance-local
- **Device instances manage their own locking** — each driver instance is responsible for thread safety
- **AlpacaHTTP uses thread-per-connection** — each accepted socket spawns a worker thread
- **Higher layers manage concurrency** — AlpacaCore provides thread-safe primitives, but concurrency is managed by AlpacaHTTP

## Error handling

- **Single exception type**: `AlpacaException` (derived from `std::runtime_error`)
- **No error-code return values** — use exceptions for error handling
- **AlpacaHTTP maps exceptions** to Alpaca error numbers in HTTP responses

## Units and conventions

| Quantity | Unit |
|----------|------|
| Exposure | seconds |
| Angles | degrees |
| RA | hours |
| Dec | degrees |
| Pixel size | microns |
| Time | UTC, `std::chrono` |
| Wavelengths | nm |

## Platform support

- **Debian 13 (Trixie)** on arm64 — the only supported architecture
- arm64 targets: Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate
- 64-bit only — no 32-bit, no amd64/x86_64
- Build scripts and CMake hard-fail on non-arm64 hosts

## Further reading

- [Development Guide](development.md) — building, testing, writing drivers, Claude Code skills
- [Troubleshooting](troubleshooting.md) — common build and runtime issues
- [ASCOM Alpaca API Specification](https://ascom-standards.org/api/)
