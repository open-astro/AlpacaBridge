# AlpacaCore

A vendor-neutral C++ core library implementing ASCOM Alpaca device models and behavior.

## Overview

**AlpacaCore** is a powerful system for astrophotography that leverages AI-assisted development to build a comprehensive, vendor-neutral device driver framework. It is a protocol-level library that:

- Implements ASCOM Alpaca device models and behavior
- Contains **no HTTP / REST / sockets**
- Avoids platform-specific hacks
- Exposes a clean API that higher-level servers (e.g. **AlpacaHTTP** inside **AlpacaOS**) use
- Follows the [ASCOM Alpaca API specification](https://ascom-standards.org/api/)

The goal is to build a **flawlessly engineered**, **well-documented**, and **stable** core that the astro community can rely on for 10+ years.

## AI-Assisted Development

AlpacaCore is built using AI-assisted development following proper programming standards and best practices. The project leverages AI to:

- Generate driver implementations following established architectural patterns
- Create vendor SDK wrappers that isolate complexity
- Maintain consistent code style and documentation
- Accelerate development while ensuring code quality

**Important**: While AI assists in development, all code must be thoroughly tested and verified. Contributors must ensure everything works as expected before submitting changes. See the [Contributing](#contributing) section for testing requirements.

## Features

- Modern C++20 implementation
- Vendor-neutral device driver interfaces
- Thread-safe design
- Comprehensive error handling
- Unit conversion utilities
- Minimal dependencies (standard library only in core)
- **AI-assisted driver development** - Build any driver you need with AI guidance

## Building Your Own Drivers

AlpacaCore makes it easy for anyone to build drivers for any vendor device using AI assistance. The system follows a clean three-layer architecture that isolates vendor SDK complexity:

1. **Pure Virtual Interface** - Standard device driver API
2. **SDK Wrapper Layer** - Clean C++ interface wrapping vendor SDKs
3. **Vendor Implementation** - Concrete driver using the wrapper

### How It Works

1. **Place your vendor SDK** in the `external/` directory (see [external/README.md](external/README.md) for details)
2. **Use AI to build your driver** - Reference the comprehensive driver build guide and let AI help you:
   - Create the SDK wrapper layer
   - Implement the driver following AlpacaCore patterns
   - Configure CMake for your vendor SDK
3. **Test thoroughly** - Verify all functionality works as expected

**📚 For complete driver development guide, see [Driver Development Guide](docs/development/driver-development.md)**

The guide includes:
- Three-layer architecture overview
- Step-by-step driver implementation
- CMake configuration templates
- Testing your driver
- Troubleshooting guides

**Anyone can build any driver they need** - simply place the vendor SDK in `external/` and work with AI to create the driver following the established patterns.

## Supported Device Types per Alapaca v1 OAS 3.1

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

For detailed build instructions, prerequisites, build options, and troubleshooting, see the [Building Guide](docs/building/building.md).

**Quick Start**:
```bash
mkdir build && cd build
cmake ..
cmake --build .
```

See the [Documentation Index](docs/README.md) for all available guides.

## License

Server Side Public License v1 (SSPL v1)

See [LICENSE](LICENSE) for details.

## Contributing

AlpacaCore follows proper programming standards with AI-assisted development. When contributing to the project:

- **Test thoroughly** - All code must be tested and verified to work as expected
- **Verify functionality** - Ensure all features work correctly before submitting
- **Follow coding standards** - Maintain consistency with existing code patterns
- **Document changes** - Update documentation and changelog as needed

**Critical**: While AI assists in code generation, contributors are responsible for:
- Comprehensive testing of all functionality
- Verification that code works correctly in real-world scenarios
- Ensuring proper error handling and edge cases are covered
- Confirming compatibility with existing codebase

See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed guidelines and development workflow.

