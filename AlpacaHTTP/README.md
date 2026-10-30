# AlpacaHTTP

AlpacaHTTP is the HTTP/1.1 + JSON transport layer for AlpacaCore, implementing the ASCOM Alpaca protocol over HTTP.

## Overview

AlpacaHTTP provides:
- HTTP/1.1 server with keep-alive support
- JSON request/response handling
- Alpaca Discovery (UDP broadcast on port 32227, plus optional multicast)
- URL routing for Alpaca API endpoints
- Transaction ID management
- Error mapping to Alpaca specification
- Configuration management

## Requirements

- C++20 compiler
- CMake 3.20+ (AlpacaCore needs 3.20; AlpacaHTTP alone needs 3.18)
- nlohmann/json
- AlpacaCore library

## Building

```bash
mkdir build
cd build
cmake ..
cmake --build . --parallel
```

To force a clean rebuild:
```bash
cmake --build . --clean-first --parallel
```

### Build Options

- `ALPACAHTTP_BUILD_TESTS`: Enable test suite (default: ON)
- `ALPACAHTTP_ENABLE_DISCOVERY`: Enable Alpaca Discovery (default: ON)

## Configuration

The server reads one YAML file: the path given as its first argument, or else `config/default.yaml` in the working directory, or else `AlpacaHTTP/config/default.yaml`. The package service runs in `/var/lib/alpacabridge`, so it reads `/var/lib/alpacabridge/config/default.yaml`. The environment variables `ALPACAHTTP_PORT`, `ALPACAHTTP_DISCOVERY`, `ALPACAHTTP_DISCOVERY_PORT` and `ALPACAHTTP_LOG_LEVEL` then override the file. `Config::apply_environment_overrides` in `src/core/config.cpp` reads further `ALPACAHTTP_*` variables for the other settings.

See `config/default.yaml` for available options. The `http` section also takes `max_connections` and `keep_alive_lifetime_seconds`.

## Usage

```cpp
#include <alpacahttp/server.h>
#include <alpacahttp/config.h>

int main() {
    alpacahttp::Config config;
    config.load("config/default.yaml");
    
    alpacahttp::Server server(config);
    server.start();
    
    return 0;
}
```

## API Endpoints

### Management API

- `GET /management/v1/description` - Server description
- `GET /management/v1/configureddevices` - List configured devices that registered
- `GET /management/v1/faileddevices` - Web UI extension (not an ASCOM path): saved devices that failed to load, in the `configureddevices` row shape with `LoadError: true`, the `UniqueID` stored in the entry (decision 0018) and the saved `Config`

### Device API

- `GET /api/v1/{devicetype}/{devicenumber}/{method}` - Get device property
- `PUT /api/v1/{devicetype}/{devicenumber}/{method}` - Set device property or call method

## License

GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later), with an additional permission allowing combination with proprietary device-vendor SDKs — see the vendor-SDK linking exception appended to the license text.
