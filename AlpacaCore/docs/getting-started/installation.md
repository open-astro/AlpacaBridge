# Installation

This guide covers installing prerequisites for building AlpacaCore on Linux (Debian 13 Trixie, NUC x64, and Raspberry Pi 4/5 ARM64).

## Prerequisites

### Linux

1. **Install build tools**:
   ```bash
   # Debian/Ubuntu
   sudo apt-get update
   sudo apt-get install build-essential cmake

   # Fedora/RHEL
   sudo dnf install gcc-c++ cmake
   ```

2. **Install Catch2** (for testing):
   ```bash
   # Debian/Ubuntu
   sudo apt-get install catch2

   # Or build from source: https://github.com/catchorg/Catch2
   ```

## Verification

After installation, verify your setup:

```bash
# Check CMake version (should be 3.20 or later)
cmake --version

# Check compiler
g++ --version   # or clang++ --version
```

## Next Steps

Once prerequisites are installed:

1. See [Building AlpacaCore](../building/building.md) for build instructions
2. See [Driver Development](../development/driver-development.md) if you want to build vendor-specific drivers
3. See [Testing Guide](../development/testing.md) for information about running tests

## Troubleshooting

If you encounter issues during installation, see the [Troubleshooting Guide](../building/troubleshooting.md).
