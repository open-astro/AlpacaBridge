# AlpacaCore

A vendor-neutral C++ core library implementing ASCOM Alpaca device models and behavior.

## Overview

**AlpacaCore** is a protocol-level library that:

- Implements ASCOM Alpaca device models and behavior
- Contains **no HTTP / REST / sockets**
- Avoids platform-specific hacks
- Exposes a clean API that higher-level servers (e.g. **AlpacaHTTP** inside **AlpacaOS**) use
- Follows the [ASCOM Alpaca API specification](https://ascom-standards.org/api/)

The goal is to build a **flawlessly engineered**, **well-documented**, and **stable** core that the astro community can rely on for 10+ years.

## Features

- Modern C++20 implementation
- Vendor-neutral device driver interfaces
- Thread-safe design
- Comprehensive error handling
- Unit conversion utilities
- Minimal dependencies (standard library only in core)

## Supported Device Types

- Camera
- Telescope
- Filter Wheel
- Focuser
- Rotator
- Dome
- Shutter
- Switch
- CoverCalibrator
- ObservingConditions
- SafetyMonitor

## Building

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

### Build Options

- `ALPACACORE_BUILD_TESTS`: Build unit tests (default: ON)
- `ALPACACORE_ENABLE_ZWO`: Enable ZWO vendor support (default: OFF)
- `ALPACACORE_ENABLE_QHY`: Enable QHY vendor support (default: OFF)
- `ALPACACORE_ENABLE_IOPTRON`: Enable iOptron vendor support (default: OFF)

## License

Server Side Public License v1 (SSPL v1)

See [LICENSE](LICENSE) for details.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

