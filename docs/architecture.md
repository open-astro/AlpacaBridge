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

Drivers that talk to hardware via a vendor C library use an **SDK wrapper** (QHY, Player One, SVBONY; ZWO and ToupTek for their cameras/focusers). Drivers that talk over serial, network, or GPIO use a **protocol wrapper** (iOptron, SynScan, Celestron, Losmandy Gemini; ZWO for the AM mount and ASIAIR power Switch; ToupTek for the StellaVita Switch). ZWO and ToupTek therefore use both — hence "SDK + protocol wrapper" in the table above.

#### Layer 3: Vendor implementation

**Location**: `src/vendors/<vendor>/<vendor>_<device>_driver.cpp`

- Concrete driver implementation
- Uses wrapper layer, never touches raw SDK directly
- Implements all pure virtual methods from Layer 1

See the [Development Guide](development.md) for step-by-step implementation.

### Vendor drivers

| Vendor | Device Types | Wrapper Type | Status |
|--------|-------------|--------------|--------|
| ZWO | Camera, Focuser (EAF), Rotator (CAA), FilterWheel (EFW), Switch (dew heater, ASIAIR power), Telescope (AM mount) | SDK + protocol wrapper | Production |
| QHY | Camera, FilterWheel (integrated CFW, e.g. miniCam8M) | SDK wrapper | Production |
| Player One | Camera, FilterWheel (Phoenix Wheel), Switch (dew heater + fan) | SDK wrapper | Production |
| SVBONY | Camera | SDK wrapper | Production |
| ToupTek | Camera (incl. cooled + High Full Well), Focuser (AAF), FilterWheel (AFW-M), Switch (camera thermal: dew heater + fan; StellaVita power) | SDK + protocol wrapper | Production |
| iOptron | Telescope, Switch (iMate PowerBox), Focuser (iEAF / iAFS2/3), FilterWheel (iEFW) | Protocol wrapper | Production |
| SynScan | Telescope | Protocol wrapper | Production |
| SkyWatcher (direct MC) | Telescope (Wave 100i/150i, AZ-GTi class, motor controller protocol over USB or WiFi UDP) | Protocol wrapper | Production |
| Celestron | Telescope | Protocol wrapper | Production |
| OnStep | Telescope (generic DIY/retrofit mounts) | Protocol wrapper | Production |
| Losmandy Gemini | Focuser, CoverCalibrator (Astro Flat Panel Cover Lite, Astro Automatic FlatPanel v2, Motorized Flat Panel V3) | Protocol wrapper | Production |
| WandererAstro | CoverCalibrator (WandererCover V4), Rotator (WandererRotator Mini V1/V2), FilterWheel (SFW50/SFW50S/SFW36S), Switch (WandererBox Pro V3) | Protocol wrapper | Production |
| Astroasis | Focuser (Oasis Focuser) | Protocol wrapper | Production |
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

### WiFi manager and privileged operations

The WiFi card (3.4.0) is backed by `src/util/wifi_manager.cpp`, which drives
NetworkManager over the system D-Bus using sd-bus (libsystemd) in-process:
no subprocesses, no sudo. The API contract is documented in `wifi-api.md`;
routes live under `/management/v1/wifi/` in the router.

Privileged operations use two mechanisms, both scoped tightly:

- **Polkit**: the .deb ships
  `/usr/share/polkit-1/rules.d/50-alpacabridge-wifi.rules`, granting only the
  `alpacabridge` service user exactly six NetworkManager actions
  (`wifi.scan`, `settings.modify.system`, `network-control`,
  `enable-disable-wifi`, `wifi.share.protected`, `wifi.share.open`). The
  `wifi.share.*` pair is required for shared-mode (hotspot) activation.
  Requires the `polkitd` package on the image.
- **Ambient capabilities**: the systemd unit grants
  `CAP_SYS_TIME` (synctime endpoint, `clock_settime`) and `CAP_NET_ADMIN`
  (wifi country endpoint, nl80211 `REQ_SET_REG` — the `iw reg set`
  equivalent). Ambient grants work despite `NoNewPrivileges=true` because
  systemd applies them at exec; `CapabilityBoundingSet` is limited to the
  same two.

State-changing WiFi requests additionally carry a CSRF guard (browser
`Origin` header must match `Host`, else 403); the management surface is
otherwise unauthenticated per the trusted-LAN model. The persisted wifi
country lives in `/var/lib/alpacabridge/config/wifi_country` and is
re-applied at daemon startup in `main()` before NetworkManager's boot-time
hotspot autoconnect.

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
|   |   |   +- gemini/ wandererastro/ weewx/ bisque/
|   |   +- management/               # Device registry, discovery
|   +- external/                      # Vendor SDKs and protocol docs
|   |   +- ZWO/ QHY/ PlayerOne/      # SDK libraries
|   |   +- SVBONY/ ToupTek/
|   |   +- iOptron/ SynScan/         # Protocol documentation
|   |   +- Celestron/ Losmandy/
|   |   +- WandererAstro/            # WandererCover V4 serial protocol docs
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
