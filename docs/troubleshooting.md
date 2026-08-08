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
2. Check that the SDK library is arm64 (`.a`/`.so` under the vendor's armv8 or arm64 subdir)
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

### Device clock resets to a stale time after reboot

**Symptom**: the SBC's clock shows an old date (e.g. 2017 or 2025) after a reboot, so Alpaca timestamps (and ConformU checks like `LastExposureStartTime`) are wrong even though the PC clock is fine.

**Cause**: a headless SBC with no internet has no NTP source, and if its hardware RTC is unset or its battery is dead, the kernel boots with the stale RTC value.

**Fix — set the clock and install an NTP client** (per the [OpenAstro SBC install guide](https://www.openastro.net/docs/sbc-install/)):

```sh
sudo date -s "YYYY-MM-DD HH:MM:SS"      # current time, UTC
sudo apt install systemd-timesyncd -y    # or: sudo apt install chrony -y
sudo timedatectl set-ntp true
sudo apt install util-linux-extra -y     # provides hwclock
sudo hwclock -w                          # persist the correct time to the RTC
timedatectl status                       # expect: System clock synchronized: yes
```

**If the SBC has no internet at all**: use `scripts/sync-clock.sh` from an internet-connected workstation (e.g. your laptop) whenever you connect over SSH — it pushes the workstation's current time to the SBC (and optionally persists it to the RTC with `--rtc`):

```sh
scripts/sync-clock.sh astro@192.168.168.1 --rtc
```

This keeps Alpaca timestamps correct even with no NTP reachable. The hardware RTC persists the time across reboots (as long as the RTC battery holds).

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
