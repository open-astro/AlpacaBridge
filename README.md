# AlpacaHTTP

AlpacaHTTP is the HTTP/1.1 + JSON transport layer for AlpacaCore, implementing the ASCOM Alpaca protocol over HTTP.

## Overview

AlpacaHTTP provides:
- HTTP/1.1 server with keep-alive support
- JSON request/response handling
- Alpaca Discovery (UDP multicast)
- URL routing for Alpaca API endpoints
- Transaction ID management
- Error mapping to Alpaca specification
- Configuration management

## Requirements

- C++20 compiler
- CMake 3.15+
- Boost.Beast (or cpp-httplib as alternative)
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
- `ALPACAHTTP_USE_BOOST_BEAST`: Use Boost.Beast for HTTP (default: ON)
- `ALPACAHTTP_ENABLE_DISCOVERY`: Enable Alpaca Discovery (default: ON)

## Configuration

Configuration is loaded from:
1. `config/default.yaml`
2. User config file (optional)
3. Environment variables (optional)

See `config/default.yaml` for available options.

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
- `GET /management/v1/configureddevices` - List configured devices

### Device API

- `GET /api/v1/{devicetype}/{devicenumber}/{method}` - Get device property
- `PUT /api/v1/{devicetype}/{devicenumber}/{method}` - Set device property or call method

## License

Server Side Public License v1
