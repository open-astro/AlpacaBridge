# Troubleshooting

Common build and runtime issues for AlpacaBridge.

## Build issues

### CMake not found

**Error**: `cmake: command not found`

**Solution**: Install CMake via `sudo apt install cmake`. Verify: `cmake --version` (3.20 or later required).

### Test framework not found

**Warning**: `No test framework found. Install Catch2 or doctest to build tests.`

**Solution**: `sudo apt install catch2`, or disable tests: `cmake .. -DALPACACORE_BUILD_TESTS=OFF`

### Vendor SDK not found

**Error**: `FATAL_ERROR: <Vendor> SDK not found`

**Solution**:
1. Ensure the vendor SDK is placed in `AlpacaCore/external/`
2. Verify the SDK folder structure matches what the CMakeLists.txt expects
3. Ensure you enabled the vendor: `-DALPACACORE_ENABLE_<VENDOR>=ON`

### Linker errors

**Error**: `undefined reference to...`

**Solution**:
1. Verify vendor SDK libraries are in the correct location
2. Check that the SDK library matches your platform (x64 or arm64)
3. Ensure CMake found the SDK correctly (check CMake output)
4. Verify the library file exists and is the correct format (`.a` for static, `.so` for shared)

### Compiler version issues

**Error**: C++20 features not supported

**Solution**: Update to GCC 10+ or Clang 10+. Verify: `g++ --version`

### Missing system libraries

**Solution**: Install all build dependencies:

```sh
sudo apt install build-essential cmake g++ \
    libusb-1.0-0-dev libudev-dev \
    nlohmann-json3-dev libcurl4-openssl-dev \
    catch2
```

## Runtime issues

### Serial port connection fails

**Symptom**: Device (mount, focuser) does not connect when using a serial port such as `/dev/ttyUSB0`.

**Checks**:

1. **Port path in config**: The device must have Connection type set to Serial/USB and Port path set to the actual device (e.g., `/dev/ttyUSB0`). In the Web UI: add/edit the device, choose Serial/USB, and enter the port path.

2. **Permissions**: Your user must be in the `dialout` group:
   ```sh
   sudo usermod -aG dialout $USER
   ```
   Log out and back in. Verify: `groups` should list `dialout`.

3. **Device present and not in use**: Check the port exists: `ls /dev/ttyUSB*` or `ls /dev/ttyACM*`. Ensure no other process has the port open.

4. **Server logs**: When connection fails, the server logs a clear error. Run the server from a terminal or check its log output to see the exact reason.

See [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) for driver-specific notes.

### Permission denied

**Solution**:
1. Ensure you have write permissions in the build directory
2. Don't build in system directories — use a local `build/` directory
3. For USB devices, add udev rules and join the `dialout` group

## Clean build

If all else fails, try a clean build:

```sh
rm -rf build
mkdir build
cd build
cmake ..
cmake --build . --parallel
```

## Getting help

Open an issue on GitHub with:
- Your operating system and version
- Compiler version (`g++ --version`)
- Full CMake output
- Any relevant error messages
