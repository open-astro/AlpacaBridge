# Troubleshooting

Common build issues and solutions for AlpacaCore.

## CMake Not Found

**Error**: `cmake: command not found`

**Solution**: 
- Install CMake using your platform's package manager or download from https://cmake.org/download/
- See [Installation Guide](../getting-started/installation.md) for platform-specific instructions
- Verify CMake is in your PATH: `cmake --version`

## Test Framework Not Found

**Warning**: `No test framework found. Install Catch2 or doctest to build tests.`

**Solution**: 
- Install Catch2: 
  - macOS: `brew install catch2`
  - Linux: Use your package manager (e.g., `sudo apt-get install catch2`)
  - Windows: Use vcpkg or download from https://github.com/catchorg/Catch2
- Or install doctest as an alternative
- Or disable tests: `cmake .. -DALPACACORE_BUILD_TESTS=OFF`

## Vendor SDK Not Found

**Error**: `FATAL_ERROR: <Vendor> SDK not found`

**Solution**: 
1. Ensure the vendor SDK is placed in the `external/` directory
   - See [external/README.md](../../external/README.md) for details on SDK placement
   - SDKs should be extracted directly into `external/` (e.g., `external/ASICamera2_SDK/`)
2. Verify the SDK folder structure matches what the CMakeLists.txt expects
3. Check that the SDK folder name matches the expected pattern
4. Ensure you've enabled the vendor option: `-DALPACACORE_ENABLE_<VENDOR>=ON`

## Linker Errors

**Error**: `undefined reference to...`

**Solution**:
1. Verify vendor SDK libraries are in the correct location
2. Check that the SDK library matches your platform (x64, ARM, etc.)
3. Ensure CMake found the SDK correctly (check CMake output for SDK location)
4. Verify the library file exists and is the correct format (`.a` for static, `.so`/`.dylib` for shared)
5. Check that all required SDK dependencies are available

## Build Errors on macOS

**Error**: Framework or library not found

**Solution**:
1. Ensure Xcode Command Line Tools are installed: `xcode-select --install`
2. Verify CMake can find system libraries
3. Check that you're using a compatible compiler version
4. For framework issues, verify the framework path in CMakeLists.txt

## Build Errors on Linux

**Error**: Missing system libraries

**Solution**:
1. Install development packages: `sudo apt-get install build-essential`
2. Install any missing system dependencies
3. Check that your compiler version is compatible (C++20 support required)

## Build Errors on Windows

**Error**: Visual Studio not found or compiler errors

**Solution**:
1. Ensure Visual Studio 2019 or later is installed with C++ development tools
2. Use the Developer Command Prompt for Visual Studio
3. Verify CMake generator matches your Visual Studio version
4. Check that the Windows SDK is installed

## Compiler Version Issues

**Error**: C++20 features not supported

**Solution**:
1. Update your compiler to a version that supports C++20:
   - GCC 10 or later
   - Clang 10 or later
   - MSVC 2019 or later (with `/std:c++20`)
2. Verify compiler version: `g++ --version` or `clang++ --version`

## Permission Errors

**Error**: Permission denied when building

**Solution**:
1. Ensure you have write permissions in the build directory
2. Don't build in system directories - use a local `build/` directory
3. On Linux/macOS, avoid using `sudo` for builds

## Still Having Issues?

If you're still experiencing problems:

1. Check the [Building Guide](building.md) for detailed build instructions
2. Review CMake output for specific error messages
3. Ensure all prerequisites are installed (see [Installation Guide](../getting-started/installation.md))
4. Try a clean build:
   ```bash
   rm -rf build
   mkdir build
   cd build
   cmake ..
   cmake --build .
   ```
5. Open an issue on GitHub with:
   - Your operating system and version
   - Compiler version
   - Full CMake output
   - Any relevant error messages

