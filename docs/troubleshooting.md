# Troubleshooting

Common build and runtime issues for AlpacaBridge.

## Build issues

### CMake not found

**Error**: `cmake: command not found`

**Solution**: Install CMake via `sudo apt install cmake`. Verify: `cmake --version` (3.20 or later required).

### Test framework not found

**Warning**: `Catch2 not found. Install Catch2 (v2 or v3) to build AlpacaCore tests.`

**Solution**: `sudo apt install catch2`. A manual CMake configure can opt out instead with
`cmake .. -DALPACACORE_BUILD_TESTS=OFF`, but `run_all_tests.sh` and `scripts/ci_preflight.sh`
always build the tests, so Catch2 is required for those.

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
sudo date -u -s "YYYY-MM-DD HH:MM:SS"   # current time, UTC (-u: parse as UTC, not local)
sudo apt install systemd-timesyncd -y    # or: sudo apt install chrony -y
sudo timedatectl set-ntp true
sudo apt install util-linux-extra -y     # provides hwclock
sudo hwclock -w                          # persist the correct time to the RTC
timedatectl status                       # expect: System clock synchronized: yes
```

**If the SBC has no internet at all**: connecting a telescope from a client that sends `UTCDate` (NINA, SkySafari and PHD2 all do on connect) sets the SBC's clock by itself, as long as `sync_system_clock_from_clients` is left on; the Server Info panel's Clock row shows which source the clock currently has. To set it by hand instead, use the **Sync Time** button in the AlpacaBridge web portal (`http://<sbc>:6800/` → top-right server actions) — it sets the SBC's clock from the browser's time. Or use `scripts/sync-clock.sh` from an internet-connected workstation (e.g. your laptop) whenever you connect over SSH — it pushes the workstation's current time to the SBC (and optionally persists it to the RTC with `--rtc`):

```sh
scripts/sync-clock.sh astro@192.168.168.1 --rtc
```

This keeps Alpaca timestamps correct even with no NTP reachable. The hardware RTC persists the time across reboots (as long as the RTC battery holds).

**If the SBC does have NTP and a client's clock is wrong**: on the Sky-Watcher direct motor-controller driver, a client's `UTCDate` write is still reported back to that client verbatim, as the ASCOM contract requires, but it no longer feeds the pointing math. `SiderealTime`, `DestinationSideOfPier` and every goto use the SBC's own clock whenever the kernel reports it disciplined. The discipline is sampled when the client writes, and, while a client offset is armed on a host that was undisciplined at the write, re-sampled at most once every 30 s on the pointing path (one `adjtimex` read, no device I/O; issue #405). So a host that acquires NTP discipline later, whether by a step or by NTP slewing a clock that was already close, stops honouring the client's offset for pointing within about 30 s and logs an INFO line saying so; the `UTCDate` readback keeps honouring the client until it writes again. A host that was disciplined at the write and loses discipline afterwards keeps ignoring the offset, which is the safe direction. If pointing is wrong in that state, fix the client's clock; the driver will not follow it.

### The mount stopped by itself mid-slew and the log says "Client-silence motion watchdog"

**Symptom**: a goto or a `MoveAxis` command stops on its own, with an `ERROR`-level log line like `Client-silence motion watchdog: no request reached <name> #<n> for 34 s (limit 30 s) while it was slewing; stopping motion, Connected left true.` The device stays `Connected` — the client can still see and reconnect to it.

**What happened**: no Alpaca request reached that telescope for longer than the configured limit while it was actively slewing (a goto, a park, a find-home, or a `MoveAxis` at a nonzero rate). AlpacaBridge treats that as a client that crashed, a host that went to sleep, or a network that dropped — with nobody left watching a moving axis, it stops the axes itself (`AbortSlew`, then `MoveAxis(axis, 0)` on each) rather than let the mount keep driving with no supervision. This is a safety feature (issue #547), not a bug — it exists specifically so a live mount does not keep slewing unattended after the thing that commanded the slew has gone away.

**It will never fire on a mount that is only tracking or guiding.** The watchdog arms only while `Slewing` is true (a goto/park/home/MoveAxis in progress); ordinary sidereal tracking and `PulseGuide` never set `Slewing`, so a quietly tracking mount with a disconnected client is left alone.

**Any request to the device resets the timer**, including a client's own routine `Slewing` polls — normal NINA/PHD2/ConformU polling (every few seconds) never comes close to the limit. A synchronous request (a plain, non-async `SlewToCoordinates`) that itself blocks past the limit is also covered: it counts as activity for its whole duration, not just when it started, so a long deliberate goto does not get aborted out from under the very client that is waiting on it. A genuine trip means requests really did stop arriving for that long — which also means a request from any OTHER client still addressing the same telescope (a planetarium app polling position, say) keeps the watchdog disarmed even after the client that started the motion has gone away.

**To change the limit or turn it off**: set `server.motion_watchdog_seconds` in your config (default 30; 0 disables it) or the `ALPACAHTTP_MOTION_WATCHDOG_SECONDS` environment variable. A slower-polling client may want a longer value; disabling it removes this backstop entirely, so only do that if you have another way to guarantee a hung client's slew gets stopped.

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
