# Installation

This guide covers installing prerequisites for building AlpacaCore on macOS, Linux, and Windows.

## Prerequisites

### macOS

1. **Install Xcode Command Line Tools**:
   ```bash
   xcode-select --install
   ```

2. **Install CMake** (if not already installed):
   ```bash
   # Using Homebrew (recommended)
   brew install cmake
   
   # Or download from https://cmake.org/download/
   ```

3. **Verify installation**:
   ```bash
   cmake --version  # Should be 3.20 or later
   clang++ --version  # Should show Apple Clang
   ```

4. **Install Catch2** (for testing):
   ```bash
   brew install catch2
   ```

### Linux

1. **Install build tools**:
   ```bash
   # Ubuntu/Debian
   sudo apt-get update
   sudo apt-get install build-essential cmake
   
   # Fedora/RHEL
   sudo dnf install gcc-c++ cmake
   ```

2. **Install Catch2** (for testing):
   ```bash
   # Ubuntu/Debian
   sudo apt-get install catch2
   
   # Or build from source: https://github.com/catchorg/Catch2
   ```

### Windows

1. **Install Visual Studio 2019 or later** with C++ development tools
   - Include "Desktop development with C++" workload
   - Ensure CMake tools are included

2. **Install CMake** from https://cmake.org/download/
   - Or use the CMake tools included with Visual Studio

3. **Install Catch2** (for testing):
   - Use vcpkg: `vcpkg install catch2`
   - Or download from https://github.com/catchorg/Catch2

4. **Use Developer Command Prompt** or ensure CMake is in PATH

## Verification

After installation, verify your setup:

```bash
# Check CMake version (should be 3.20 or later)
cmake --version

# Check compiler
# macOS/Linux:
clang++ --version  # or g++ --version
# Windows:
cl.exe
```

## Next Steps

Once prerequisites are installed:

1. See [Building AlpacaCore](../building/building.md) for build instructions
2. See [Driver Development](../development/driver-development.md) if you want to build vendor-specific drivers
3. See [Testing Guide](../development/testing.md) for information about running tests

## Troubleshooting

If you encounter issues during installation, see the [Troubleshooting Guide](../building/troubleshooting.md).

