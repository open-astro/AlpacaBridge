# Building AlpacaCore

This guide covers building AlpacaCore from source on macOS, Linux, and Windows.

## Prerequisites

See [Installation Guide](../getting-started/installation.md) for detailed prerequisites and installation instructions.

## Quick Start

### macOS / Linux

```bash
mkdir build
cd build
cmake ..
cmake --build . --parallel
```

### Windows

```cmd
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release --parallel
```

## Build Options

AlpacaCore supports several CMake options to customize the build:

### Test Framework

- `ALPACACORE_BUILD_TESTS`: Build unit tests (default: ON)
  - Requires Catch2 or doctest to be installed
  - See [Testing Guide](../development/testing.md) for details

### Vendor Support

Enable vendor-specific drivers by setting the appropriate CMake option:

- `ALPACACORE_ENABLE_ALL_VENDORS`: Enable all implemented vendor drivers (default: OFF)

- `ALPACACORE_ENABLE_ZWO`: Enable ZWO vendor support (default: OFF)
  - Uses the vendored ZWO SDK subset in `external/ASI_Camera_SDK/`
  - See [external/README.md](../../external/README.md) for SDK notes

- `ALPACACORE_ENABLE_QHY`: Enable QHY vendor support (default: OFF)
  - Requires QHY SDK in `external/` directory

- `ALPACACORE_ENABLE_IOPTRON`: Enable iOptron vendor support (default: OFF)
  - Requires iOptron SDK in `external/` directory

## Build Examples

### Building with Vendor Support

```bash
mkdir build
cd build
cmake .. -DALPACACORE_ENABLE_IOPTRON=ON
cmake --build . --parallel
```

### Building without Tests

```bash
mkdir build
cd build
cmake .. -DALPACACORE_BUILD_TESTS=OFF
cmake --build . --parallel
```

### Building with Multiple Vendors

```bash
mkdir build
cd build
cmake .. \
  -DALPACACORE_ENABLE_ZWO=ON \
  -DALPACACORE_ENABLE_IOPTRON=ON
cmake --build . --parallel
```

## Build Output

After building, you'll find:

- **Library**: `build/libalpacacore.*` (static or shared, name varies by platform)
- **Tests**: `build/tests/alpacacore_tests` (if tests enabled)
- **Examples**: `build/examples/` (if examples enabled)

## Running Tests

After building with tests enabled:

```bash
cd build
ctest
```

For more information about testing, see the [Testing Guide](../development/testing.md).

## Troubleshooting

If you encounter build issues, see the [Troubleshooting Guide](troubleshooting.md).

## Next Steps

After building AlpacaCore:

- See [README.md](../../README.md) for an overview of the project
- See [CONTRIBUTING.md](../../CONTRIBUTING.md) for development guidelines
- See [Driver Development Guide](../development/driver-development.md) for building custom drivers
- Check the `examples/` directory for usage examples
