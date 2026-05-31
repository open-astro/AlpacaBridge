# Agent Instructions

This file is the single source of truth for agent behavior in this repository.

## Repository Structure and Build Output

- Keep build/output folders inside the owning project directory:
  - `AlpacaCore/build*`
  - `AlpacaHTTP/build*`
- Never create root-level ad-hoc build directories (examples to avoid: `build-synscan`, `build-temp`, `cmake-build-*` at repo root) unless explicitly requested.
- Keep generated artifacts out of source trees and avoid tracked build-system output files (Makefiles, CMake cache files, etc.) outside approved build folders.

## Core Architecture

- `AlpacaCore` is vendor-neutral Alpaca logic and device behavior only:
  - No HTTP/REST/sockets/JSON transport code.
  - Vendor SDKs isolated to `external/` + `src/vendors/<vendor>/` + `include/alpacacore/vendor/<vendor>/`.
- `AlpacaHTTP` is transport/routing/config/discovery only:
  - No vendor SDK use.
  - No duplicated device logic from `AlpacaCore`.
  - Web UI assets live in `AlpacaHTTP/web/` (HTML, CSS, JS). These are served by the HTTP server and packaged into the `.deb` at `/usr/share/alpacabridge/web/`.
- Call flow is always:
  - `AlpacaHTTP -> AlpacaCore driver -> vendor implementation`.

Supported device types (base drivers in `AlpacaCore/src/drivers/`): Camera, Telescope, FilterWheel, Focuser, Rotator, Dome, Shutter, Switch, CoverCalibrator, ObservingConditions, SafetyMonitor.

## Language, Style, and Safety

- C++20 preferred, RAII, small focused functions.
- Use `#pragma once` in headers.
- Prefer `enum class`, `std::chrono`, `std::string_view` where appropriate.
- No `using namespace std;` in headers.
- Core/driver layers should avoid heavy framework dependencies.
- License headers must remain SSPL v1 and unmodified in all source files.

## Target Architecture

- **Linux arm64 only** (ARMv8 — Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate). amd64/x86_64 is no longer supported, built, packaged, or validated. CMake, `debian/rules`, `build_and_run.sh`, and `install_alpaca_service.sh` all hard-fail on non-arm64 hosts.
- When writing driver code, follow fixed-width integer practices for protocol/SDK structs (`int32_t`, `uint16_t`, etc.) and avoid `long double`. The wider portability concerns (endianness, alignment) no longer matter for our build target, but using fixed-width types still makes wire-protocol code easier to read and harder to misread.
- ConformU validation is performed on arm64 only. Historical amd64/x64 ConformU reports have been deleted from `AlpacaCore/conformu/`.

## Driver Implementation Rules

- Use 3-layer driver pattern:
  1. Alpaca interface (`include/alpacacore/*_driver.h`)
  2. Vendor wrapper (`include/alpacacore/vendor/<vendor>/...`)
  3. Vendor implementation (`src/vendors/<vendor>/...`)
- Do not include raw vendor SDK headers outside wrapper implementation files.
- Convert vendor failures to `AlpacaException`.
- Gold-standard runtime semantics for drivers:
  - Async `connect()/disconnect()` with `get_connecting()`.
  - Synchronous `set_connected()` for compatibility.
  - Useful `get_device_state()` telemetry.
  - Clean thread/task shutdown in destructors.
- Add TODO comments where vendor protocol/SDK behavior is uncertain.

## CMake and Vendor Integration

- Guard each vendor behind explicit build options.
- When adding a vendor in `AlpacaCore/CMakeLists.txt`, always update:
  1. `option(ALPACACORE_ENABLE_<VENDOR> ...)`
  2. `ALPACACORE_ENABLE_ALL_VENDORS` logic (only if implemented)
  3. conditional `add_subdirectory(src/vendors/<vendor>)` + link
  4. install rules for vendor target
- If vendor libs are discovered by pkg-config, prefer imported targets (example: `PkgConfig::LIBUSB`) so dependent test binaries get correct link paths.
- When adding a new vendor SDK under `AlpacaCore/external/<vendor>/`, add an allowlist entry to `AlpacaCore/.gitignore` so the SDK binaries (`.a`, `.so`, `.dll`, firmware files, etc.) are not blocked by the global compiled-file ignore rules. Follow the existing pattern: `!external/<VENDOR>/**`.

## Vendor SDK Shared Library Packaging

**Every new camera vendor MUST ship its `.so` in the `.deb`, regardless of whether AlpacaBridge itself statically links the library.** Camera `.so` files are also consumed by companion projects (e.g. SmartGuider at `/home/dev/Documents/GitHub/SmartGuider/` — uses `libASICamera2.so` via `zwoasi`, `libqhyccd.so`, etc. for autoguiding) which dynamically `dlopen` them from the system library path. If the `.so` is missing or not registered with `ldconfig`, guiding fails at runtime with no warning from the AlpacaBridge build or test suite. The fact that the AlpacaBridge server binary links fine is NOT evidence that packaging is correct.

Non-camera vendors (focusers, mounts, switches, rotators) also ship their `.so` if the SDK provides one, for consistency — but the camera rule is non-negotiable.

### Mandatory Checklist — New Vendor SDK with `.so`

Do **all** of the following when adding a new vendor SDK. Skipping any step will either silently break companion projects (step 1), break CI / clean clones (step 2), or break runtime loading on installed systems (steps 3–5).

1. **Store** static (`.a`) and shared (`.so`) libraries under `AlpacaCore/external/<VENDOR>/` in the arm64 subdirectory the upstream SDK uses (commonly `lib/linux/armv8/`, `lib/linux/arm64/`, or `lib/armv8/`). Document the exact path in the vendor-specific notes section below. Do not commit x86_64/x64 SDK binaries — AlpacaBridge is arm64-only and they would only bloat the repo.
2. **Allowlist in `.gitignore`**: add `!external/<VENDOR>/**` to `AlpacaCore/.gitignore` **before** committing the SDK files. The global `*.so` ignore rule will silently drop the library from the commit otherwise. Verify with `git check-ignore -v <path-to-.so>` — the output must show the `!external/<VENDOR>/**` rule winning.
3. **`debian/rules`** — copy `.so*` to `$(STAGING)/usr/lib/alpacabridge/` in `override_dh_auto_install`, alongside existing QHY/ZWO/SVBONY entries. Add a `<VENDOR>_LIB_DIR` variable at the top of the file pointing at the arm64 SDK path.
4. **`build_and_run.sh`** — copy `.so*` to `/usr/local/lib/`, run `ldconfig`. Add inside the udev rules block alongside existing QHY/ZWO/SVBONY install logic.
5. **`install_alpaca_service.sh`** — same as `build_and_run.sh`, inside the `install_udev_rules()` function. Keep the two scripts in sync — they must install the same set of vendor libraries.

### Dynamic Linker Registration

The `.deb` ships `/etc/ld.so.conf.d/alpacabridge.conf` which adds `/usr/lib/alpacabridge` to the system library search path. The `postinst` script runs `ldconfig` so libraries are discoverable immediately after install. This is what makes companion projects (SmartGuider's `zwoasi`, `ctypes.CDLL('libtoupcam.so')`, etc.) able to find vendor libraries without setting `LD_LIBRARY_PATH`.

### SDK Version Bumps

When a vendor releases a new SDK version, update the `.so` files in `external/` and bump symlink targets (e.g. `libASICamera2.so.1.41` → `libASICamera2.so.1.42`). If the vendor's SDK path is versioned (e.g. ToupTek's `toupcamsdk.20260128/`), update the path reference in `debian/rules`, `build_and_run.sh`, `install_alpaca_service.sh`, AND `AlpacaCore/src/vendors/<vendor>/CMakeLists.txt` in the same commit.

### Verification

After wiring a new camera vendor, verify the `.so` is reachable as SmartGuider would see it:

```bash
# After ./build_and_run.sh or dpkg -i alpacabridge_*.deb:
ldconfig -p | grep <libname>                    # must list the .so
python3 -c "import ctypes; ctypes.CDLL('<libname>.so')"  # must not raise OSError
```

Failing this check means guiding will fail at runtime, no matter how green the AlpacaBridge test suite is.

## AlpacaHTTP Integration Checklist (Required for New Vendor/Device Types)

When adding a new vendor/device type in AlpacaCore, also update AlpacaHTTP:

1. **Router registration** — add vendor/device case to `Router::register_device_from_config` in `AlpacaHTTP/src/http/router.cpp`. This is the dispatch that creates driver instances from persisted JSON config.
2. **Router includes** — add `#include <alpacacore/vendor/<vendor>/<vendor>_<device>_driver.h>` at the top of `router.cpp`, guarded by `#ifdef ALPACACORE_ENABLE_<VENDOR>`.
3. **Config sanitization fields** — ensure vendor-specific config keys are preserved through sanitization.
4. **Web UI vendor dropdown** — add the vendor to the device-type dropdown in the web frontend (`AlpacaHTTP/web/app.js`) so users can select it.
5. **Web UI vendor-specific form fields** — add any vendor-specific configuration fields (e.g. serial port, camera index, connection type) to the frontend form.
6. **Frontend validation** — add any related validation logic in frontend JS.
7. **Build-flag propagation** — ensure `ALPACACORE_ENABLE_<VENDOR>` compile definitions propagate from AlpacaCore to AlpacaHTTP.
8. **Routing/config tests** — add or update tests in `AlpacaHTTP/tests/`.

Vendor registration alone is not enough for HTTP/UI visibility. All eight steps must be completed for a new vendor/device to be fully functional end-to-end.

## Debian Packaging

- Package files live in `debian/` (control, rules, changelog, copyright, service file, maintainer scripts).
- The `.deb` installs to:
  - `/usr/bin/alpacabridge` — server binary.
  - `/usr/lib/alpacabridge/` — vendor shared libraries (e.g. `libqhyccd.so`, `libASICamera2.so`).
  - `/usr/share/alpacabridge/web/` — web UI static files.
  - `/lib/firmware/qhy/` — QHY camera firmware files.
  - `/lib/udev/rules.d/` — udev rules for USB device permissions.
  - `/usr/sbin/fxload` — QHY firmware loader.
  - `/etc/alpacabridge/` — default config (`registered_devices.json`).
- When adding a new vendor with shared libraries, update `debian/rules` `override_dh_auto_install` to copy them into `$(STAGING)/usr/lib/alpacabridge/`.
- Update `debian/changelog` when cutting a release.

## Testing Requirements

- Non-trivial code must have unit tests under `AlpacaCore/tests/` or `AlpacaHTTP/tests/`.
- Build driver targets and test targets together.
- Tests should be runnable via `run_all_tests.sh`.
- Preferred test naming:
  - `test_<component>.cpp`
  - `test_<vendor>_<device>.cpp`
- Use tags to separate unit/integration/hardware behavior when applicable.
- Use Catch2 macros (`REQUIRE`, `CHECK`, `CHECK_THROWS_AS`, etc.) via the `catch2_compat.h` header.

### Required Test Cases for Every New Vendor Device Driver

Every new driver **must** ship with at least the following test cases. Use the existing tests (e.g. `test_svbony_camera.cpp`, `test_gemini_focuser.cpp`) as reference.

1. **Defaults** `"<Vendor> <Device> Driver - Defaults"` `[<vendor>][<device>][unit]`
   - Create driver with device number 0.
   - `REQUIRE` device type, device number, and `get_connected() == false`.
   - `CHECK` the default device name.
   - `CHECK` any static capability flags (e.g. `get_can_abort_exposure`, `get_can_reverse`, `get_absolute`).

2. **Device metadata** `"<Vendor> <Device> Driver - Device metadata"` `[<vendor>][<device>][unit]`
   - Create driver with a non-zero device number (e.g. 3) so `get_unique_id()` is distinguishable.
   - `CHECK` all of: `get_device_number`, `get_description`, `get_driver_info`, `get_driver_version`, `get_interface_version`, `get_unique_id`.
   - String values must match the implementation exactly — read the driver source to get the correct strings.

3. **Not connected throws / Disconnected behavior** `[<vendor>][<device>][unit]`
   - Verify that operations requiring a live connection throw `alpacacore::AlpacaException` (or return safe defaults where the driver explicitly does so — document why in a comment).
   - Cover the device's primary operations (e.g. for cameras: `get_gain`, `start_exposure`, `get_image_array`; for telescopes: `get_right_ascension`, `get_tracking`, `slew_to_target_async`).

4. **Unsupported actions** `[<vendor>][<device>][unit]`
   - `CHECK` `get_supported_actions()` is empty (unless the driver defines actions).
   - `CHECK` `can_action("anything") == false`.
   - `CHECK_THROWS_AS` for `action()`, `command_blind()`, `command_bool()`, `command_string()`.

5. **Device-specific behavior** — at least one test covering behavior unique to the device type:
   - Cameras: sub-exposure support (`get_sub_exposure_duration` / `set_sub_exposure_duration` throw if unsupported).
   - Telescopes: target coordinate validation, axis rate ranges.
   - Focusers: `get_absolute`, `get_temp_comp_available`, device state telemetry.
   - Filter wheels: names/offsets defaults, invalid position handling.
   - Switches: `get_max_switch`, invalid switch ID handling.
   - Rotators: `get_can_reverse`, device state telemetry.

### Test CMake Integration

When adding a test file for a new vendor device:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list in `AlpacaCore/tests/CMakeLists.txt`, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.
- Build and run all tests (`cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests`) before considering the driver complete.

## Logging, Threading, and Errors

- Use AlpacaCore logging sink flow; do not use ad-hoc stdout/stderr logging in runtime paths.
- Avoid global mutable state; protect shared state with mutexes.
- Use `AlpacaException` for error paths; AlpacaHTTP maps exceptions to Alpaca error responses.
- AlpacaHTTP must return Alpaca-style JSON envelopes and stable error mapping behavior.
- On-disk logging writes daily files `alpacabridge-YYYY-MM-DD.log` to `logging.directory` (default `/var/log/AlpacaBridge`, per-config override, env `ALPACAHTTP_LOG_DIRECTORY`). The sink falls back to `$XDG_STATE_HOME/AlpacaBridge/logs` (or `~/.local/state/AlpacaBridge/logs`) when the configured path is not writable. systemd unit uses `LogsDirectory=AlpacaBridge`; the deb postinst pre-creates the directory for non-systemd starts. There is no in-memory log buffer — `/management/v1/logs` reads today's daily file directly from disk.
- Retention: `logging.retention_days` (default 90, 0 = forever, env `ALPACAHTTP_LOG_RETENTION_DAYS`) auto-deletes daily files whose embedded date is older than `today − retention_days`. Pruning runs once on startup and again on day-rollover inside the file sink. Today's active file is never pruned.
- Web portal exposes `GET /management/v1/logfiles`, `GET /management/v1/logfiles/{name}[?download=1]`, and `DELETE /management/v1/logfiles/{name}`. Filenames are validated against the daily pattern to prevent path traversal. `util::read_log_file` enforces a 10 MiB per-request cap; web viewer warns and suggests download above 5 MiB.
- Log level set via `POST/PUT /management/v1/loglevel` is persisted to `config/runtime_state.json` and reapplied on the next start (overrides `default.yaml`'s `logging.level`). Delete that file to fall back to the YAML default. Persistence failures are logged at WARNING and never block the API response.
- Alpaca-style management responses (including the new logfile endpoints) return HTTP 200 even when `ErrorNumber != 0` — clients must inspect the body, not the HTTP status.

## Units and Behavior Conventions

- Exposure: seconds
- Angles: degrees
- RA: hours
- Dec: degrees
- Pixel size: microns
- Time: UTC with `std::chrono`

## Vendor-Specific Notes

### ZWO

Devices: Camera, FilterWheel, Focuser (EAF), Rotator, Switch (dew heater, ASIair Pro 12V power), Telescope (AM mount).

SDK locations: `AlpacaCore/external/ZWO/ASI_Camera_SDK/`, `EAF/`, `EFW/`, `CAA/`, `AM/`. The ASIair Pro switch driver does not use an SDK — it talks directly to the on-board Pi 4 GPIO via libgpiod v2.

- ROI sizing rules: width must be a multiple of 8 and height a multiple of 2 after binning. Keep requested sizes for Alpaca, align effective sizes down for SDK calls, and pad outputs if needed.
- Dew heater is exposed as an Alpaca Switch device (not a camera action) and is camera-dependent.
- ST4 pulse guiding should be enabled only when the SDK reports `has_st4_port`.
- PulseGuide: do not apply permanent RA/Dec offsets based on expected guide motion. If synthetic offsets are needed, keep them temporary and clear after the pulse completes to avoid double-counting mount motion.
- The ZWO and QHY SDKs both statically link libusb, causing duplicate symbol issues. The ZWO vendor `CMakeLists.txt` handles this — do not link both vendor static libs into the same binary without resolving the conflict.

#### ZWO ASIair Pro Switch (12V power ports via on-board GPIO)

End-user setup instructions live in [AlpacaCore/PowerPorts.md](AlpacaCore/PowerPorts.md); this section captures the implementation-side context.

- **Hardware reality**: ASIair Pro is a Raspberry Pi 4 (BCM2711) with a custom HAT exposing four 12V DC outputs. The stock ZWO firmware enables them at boot via `/boot/config.txt` under `[all]`: `gpio=18,12,13,26=op,dh,pu` (all four configured as output, default-high, pull-up). The boot-time `dh` flag is why all four DC ports come up powered as soon as the Pi boots — gear plugged in is "live" before any userspace runs.
- **Port-to-GPIO mapping** (Pi 4 ASIair Pro): Port 1 = GPIO 12, Port 2 = GPIO 13, Port 3 = GPIO 26, Port 4 = GPIO 18 on `/dev/gpiochip0`. Confirmed against the stock app via direct probe. Note: the order in `/boot/config.txt` (18,12,13,26) is *not* the port order.
- **Persistent state shape**: The stock ZWO `zwoair_imager` binary persists per-port settings in `~/.ZWO/ASIAIR_imager.xml` under the XPath `setting2/imager/gpio/port_N/` (zero-indexed: `port_0`..`port_3`). Each port has an `is_pwm` boolean flag. No per-port GPIO pin number is stored — the port-index→GPIO mapping is hard-coded in the stock binary. The AlpacaBridge driver makes the mapping configurable via `ports: [{gpio: N, pwm: bool}]` in the device config so it can be reused on other arm64 SBCs (e.g. RK3568-based ASIair Plus) with different wiring.
- **App role abstraction is cosmetic**: the ASIair mobile app lets users assign a *role* to each port (Mount / Camera / Focuser / Dew Heater / Flat Panel / Other). Roles "Dew Heater" and "Flat Panel" enable software PWM dimming; the others are plain on/off. Under the hood every port can do either — the "role" is just a UI tag that sets the `is_pwm` flag. The AlpacaBridge driver does not model roles; it exposes 4 ASCOM Switch channels and lets users name them however they want.
- **PWM mechanism**: Stock app uses pigpio's DMA-based software PWM at 40 kHz. AlpacaBridge uses **libgpiod v2** with per-port worker threads doing userspace soft-PWM (default 1 kHz, configurable). The lower frequency keeps the driver portable across non-RPi arm64 SBCs (DMA-based PWM is BCM-specific). Dew heaters are resistive thermal loads and don't care about audible PWM frequency — sub-1-kHz works fine.
- **libgpiod version**: The driver targets libgpiod **v2** (`libgpiod-dev (>= 2.0)`, `libgpiod3` runtime). The v2 API uses a single `gpiod_line_request*` that owns all four lines together and routes value get/set through `gpiod_line_request_set_value(request, offset, GPIOD_LINE_VALUE_ACTIVE/INACTIVE)`. Do **not** port back to v1's per-line `gpiod_line_request_output` API — that would block Trixie and the upcoming RPi OS 2025 base. If you need to support older Bullseye/Bookworm hosts, install libgpiod 2.x from backports rather than dual-targeting.
- **OS architecture gate**: AlpacaBridge is arm64-only. The factory stock ASIair Pro ships **32-bit Raspbian Buster armv7l** — our `.deb` will not install on the stock OS. Deployment requires re-imaging with Raspberry Pi OS 64-bit (Bookworm or Trixie). Once re-imaged, the stock `zwoair_imager` / `pigpiod` daemons must be disabled because they hold the GPIO lines via pigpio and would prevent libgpiod from claiming them (EBUSY on `gpiod_chip_request_lines`).
- **Default-on power-up surprise**: Because the kernel cmdline drives all four GPIO lines HIGH at boot before the AlpacaBridge daemon starts, gear plugged into the DC ports gets a few seconds of unmanaged 12V before the driver claims the lines. Users who want a different boot state must either edit `/boot/config.txt` to omit specific pins from the `gpio=` directive, or live with the brief default-on window. Document this in the install notes for users moving from stock ASIair to AlpacaBridge.
- **Disconnect leaves lines in their last-driven state — by design**: `AsiairProtocolWrapper::close()` releases the `gpiod_line_request` without first driving the four lines LOW. With the boot-time `gpio=...=op,dh,pu` directive setting pull-ups, a released line stays HIGH (12V outputs remain powered). This is intentional — it mirrors the boot-time default-on behavior and avoids surprising users with a power cycle they didn't ask for when an ASCOM client disconnects (a common transient operation during a session). Users who want a port OFF on disconnect should set it OFF in the client first. Do **not** add a "drive LOW on close" path without making it opt-in via config, because it would silently flip the policy for everyone who upgrades. This trade-off is documented user-facing in [`AlpacaCore/PowerPorts.md`](../AlpacaCore/PowerPorts.md) under "Disconnect behavior".
- **PWM worker error handling**: each `gpiod_line_request_set_value` call inside the PWM worker thread checks the return code; on failure it logs at ERROR level and sets the per-port `stop` flag so the thread exits cleanly. Without this, a kernel-side write failure (hardware fault, line revocation) would loop silently and the ASCOM API would keep reporting success while the hardware stayed dark. The PWM thread also caches the last-written value so steady-state 0% / 100% ports don't issue a kernel ioctl every period — important because at 1 kHz PWM that would be 1000 wasted syscalls per port per second.
- **Coexistence with stock app is not supported**: libgpiod and pigpio cannot share GPIO line ownership. The stock `pigpiod` daemon (started by `/etc/rc.local → /home/pi/ASIAIR/asiair.sh`) must be disabled, and the stock `zwoair_imager` must not run. There is no way to run AlpacaBridge alongside the stock ASIair app on the same device.

#### ZWO ASIair Plus Switch (RK3568 variant — kernel module ioctl, not libgpiod)

- **Hardware reality**: ASIair Plus (RK3568) is a Rockchip RK3568 SoC with a custom ZWO HAT. The four 12V DC outputs are **not** standard Linux GPIO from the user-space side — ZWO ships a custom kernel module (`pwm_gpio.ko`, author "JerryCui") that owns the device-tree `airplus-gpios` node and registers a misc-device character node at `/dev/pwm-gpio-misc` (major 10, minor 55). All port control goes through documented ioctls on that fd. This means standard `libgpiod` / `gpioget` tools see the lines as already-claimed and cannot drive them.
- **Reverse-engineered ioctl header**: vendored at `AlpacaCore/external/ZWO/asiair-plus/pwm_gpio.h`. Source: decoded from DWARF debug info on the production ZWO kernel module. Defines `gpio_level_t`, `pwm_param_t`, `work_mode_t` plus the ioctl numbers (`PWM_GPIO_SET_LEVEL`, `PWM_GPIO_SET_MODE`, `PWM_GPIO_SET_CONFIG`, etc.). Treat this header as the canonical interface contract — do **not** invent new ioctls without confirming against the module disassembly.
- **Port index mapping (kernel module side)**: indices 0..11. Indices 0, 1 = LEDs; 2 = physical button (input); **3 = "DC master enable / control signal"** (must be HIGH for the DC ports to deliver voltage); **4..7 = the four DC power ports (PWM-capable)**; 8..11 = USB2/USB3 power enables. The AlpacaBridge driver exposes only indices 4..7 as wrapper indices 0..3 — USB power and button are deliberately out of scope for the v1 Switch driver to keep the ASCOM contract focused. Document any future extension here.
- **open() is read-only — do NOT touch any kernel-side state on connect.** Two earlier attempts to "initialize" the kernel module from open() — first driving the master-enable line at kernel index 3, then explicitly setting mode + level on each DC port — both produced the same symptom: every DC port physically dropped to 0 V the moment an ASCOM client connected. The kernel module is closed-source / reverse-engineered. `open()` opens the fd and does nothing else. The constructor's optimistic cache (value=1 / 100 = "on") is left in place — matches typical boot-time behavior of all four DC ports being live. The first hardware write happens only when the ASCOM client calls `SetSwitch` or `SetSwitchValue`. Do not add any init-time write here.

- **PWM_GPIO_GET_LEVEL lies.** Empirically (probed across all 12 kernel indices on a freshly-booted device with all DC ports physically powered and the network LED lit): `GET_LEVEL` returns 0 for every index. It reads the kernel module's "last-written via SET_LEVEL" cache, which initializes to 0 at module load regardless of what pinctrl drove the pads to physically. So **never** trust `GET_LEVEL` for state introspection — the UI will silently flip to "all off" on first connect after fresh boot, even though every port is hot. Read `/sys/kernel/debug/gpio` (line directions and levels) for true state when debugging.

- **The kernel module needs SET_MODE → ENABLE → SET_LEVEL.** Reverse-engineered the hard way after observing that toggles in NINA had zero physical effect even though every ioctl returned 0. The kernel debug interface revealed the DC port GPIO lines (GPIO 146/147/149/150 on gpiochip4) were stuck as **inputs** with external pull-ups holding them HIGH, regardless of how many `SET_LEVEL` calls our driver made. The kernel module silently rejects writes to lines that aren't in OUTPUT direction (it logs `"NOT in GPIO OUTPUT mode"` via printk but returns 0 to userspace). The fix: call `PWM_GPIO_ENABLE` between `SET_MODE` and `SET_LEVEL` — internally that maps to `gpiod_direction_output_raw` and flips the line.

- **SET_LEVEL polarity is INVERTED from typical gpiod semantics.** Verified by reading `/sys/kernel/debug/gpio` after each ioctl AND physically observing a 12V flat panel on DC port 2: `SET_LEVEL(0)` resolves the line to `in hi` (input, pulled high externally) which **powers the panel ON**; `SET_LEVEL(1)` resolves to `out lo` (output driven low) which **cuts power**. So in the wrapper, ASCOM `value=1` (on) maps to ioctl `level=0`, and `value=0` (off) maps to ioctl `level=1`. This is non-standard — typical gpiod chips treat `level=1` as drive-high — and the cause is almost certainly a quirk in the closed pwm_gpio.ko's level-argument interpretation (likely `level=1` maps internally to `gpiod_direction_output_raw(0)` while `level=0` maps to `gpiod_direction_input`, judging by the kernel debug state transitions). Don't "fix" this by flipping it back — the previous polarity produced the inverted-feeling NINA behavior the user reported on 2026-05-30 ("when I flip it on it goes off") and connected gear lost power on every value-set transition.

- **The kernel module's PWM mode is unreachable from documented ioctls — we do userspace soft-PWM instead.** SET_MODE(PWM) → ENABLE → SET_CONFIG returns success on every input (GET_CONFIG echoes back the period/duty we wrote), but the kernel's hrtimer dispatch branch never actually arms — `/proc/timer_list` shows no `pwm_gpio_timer_func` scheduled regardless of ordering, period, or duty. Disassembly of `pwm_gpio_misc_ioctl` shows the `hrtimer_init` + `hrtimer_start_range_ns` call sites do exist at file offset 0x7c0–0x7ec inside the misc-ioctl dispatcher, but they're gated by a condition we cannot trigger from userspace. The closed source means we can't confirm the gate definitively (the inventory's "GPL" license claim notwithstanding — try ZWO support if source-level fixes are ever needed). The DTS also confirms there's no hardware PWM controller mapped to the airplus-gpios pins on GPIO bank 4, so even if we triggered the hrtimer branch we wouldn't get hardware PWM — just kernel-side soft-PWM. So we do soft-PWM in **userspace** instead: each PWM-enabled port runs a `pwm_loop` worker thread that uses `sleep_until` to toggle `SET_LEVEL` at the configured frequency, with the polarity-inverted convention this kernel module uses. Confirmed against the extracted stock `zwoair_imager` daemon — the binary contains `pwm_gpio_start` symbols proving they take the same software-PWM approach. Default 200 Hz (the quietest stable point on real hardware — see [[changelog_asiair_plus_pwm_tuning]] in PowerPorts.md and the comment block in `default_asiair_plus_rk3568_config`).

- **Kernel module forensics worth saving.** `objdump -t /lib/modules/4.19.219/kernel/drivers/misc/pwm_gpio.ko | grep UND` shows the kernel APIs it imports: `devm_gpio_request_one`, `devm_pinctrl_get`, `gpiod_direction_input`, `gpiod_direction_output_raw`, `gpiod_get_raw_value`, `gpiod_set_raw_value`, `gpio_to_desc`, `hrtimer_*`, `misc_register`, `of_get_named_gpio_flags`, `pinctrl_lookup_state`, `pinctrl_select_state`, `platform_driver_register`. Author tag in the strings: `JerryCui` from `/home/jerry/rk3568/rk356x_linux_210520/pwm_gpio/ko/pwm_gpio.c`. License: GPL (so source MUST be available — try ZWO support if this driver ever needs source-level fixes). Useful runtime introspection: `cat /sys/kernel/debug/gpio` (line directions/levels), `cat /sys/firmware/devicetree/base/pinctrl/airplus_gpios/airplus-ports` (the pin map), `cat /sys/firmware/devicetree/base/airplus-gpios` (the platform binding).
- **PWM is userspace soft-PWM** (same architecture as the Pi 4 ASIair Pro driver, just over a different kernel interface). Each PWM-enabled port spawns a `pwm_loop` worker thread at `open()` that uses `sleep_until` to toggle `SET_LEVEL` at the configured frequency. Threads are joined in `close()` (two-phase shutdown: signal stop under the mutex, join outside the mutex, close the fd back under the mutex — avoids the classic join-under-the-same-mutex deadlock). The kernel module's own hrtimer dispatch is unreachable from documented ioctls (see the bullet above) and GPIO bank 4 has no hardware PWM mux on the RK3568 anyway, so software PWM is the only viable path. Default frequency 200 Hz; range 1–100,000 Hz configurable via `pwmFrequencyHz`. The default was empirically tuned against real hardware: 1 kHz produced an audible whine from the device's own MOSFET driver (human ear sensitivity peaks at 2–4 kHz), 100 Hz showed intermittent scheduler-jitter flicker, ≤ 80 Hz destabilized loads with internal smoothing caps. 200 Hz was the quietest stable point.
- **Disconnect policy: same as the Pro**: `close()` releases our fd without first driving the lines LOW. The kernel module retains per-port mode + level across opens, so disconnecting from the ASCOM client does not power-cycle anything. Same rationale as the Pro — set ports OFF in the client *before* disconnecting if you want a cold release.
- **Permissions**: `/dev/pwm-gpio-misc` is created with root-only mode by the kernel module. We ship a udev rule (`AlpacaCore/external/ZWO/asiair-plus/99-zwo-asiair-plus.rules`) that grants the `gpio` group `0660` access. `build_and_run.sh` and the `.deb` postinst both install it via the existing rules-discovery loop in `external/`. The AlpacaBridge daemon user (and any human user wanting to poke at the device) must be in the `gpio` group.
- **Kernel module hard dependency**: `pwm_gpio.ko` ships only with ZWO's stock kernel build (4.19.219). Re-flashes that swap to mainline RK3568 distros (Armbian, etc.) will **not** include it, and the driver will fail to open `/dev/pwm-gpio-misc`. The flashing tool tracked at `rk-flashtool` is the supported path; document any alternatives here as they emerge.
- **Router config schema** (`switchType: "asiair-plus-rk3568"`): much simpler than the Pro because the kernel module fixes the index mapping. Fields are `devicePath` (default `/dev/pwm-gpio-misc`), `pwmFrequencyHz`, and a `ports[]` array where each entry is just `{ name, pwm }`. No `gpio` / `gpioChip` fields — they would be meaningless for this hardware.
- **Web UI naming convention**: device label is "ASIair Plus 12V Power Switch (RK3568)" — explicit because a Pi-CM4 variant (`asiair-plus-picm4`) is on the roadmap. Do not collapse the two to a bare `asiair-plus` `switchType`; the kernel-interface difference between the CM4 (libgpiod, like the Pro) and the RK3568 (kernel module, like this driver) is fundamental.

### QHY

Devices: Camera.

SDK location: `AlpacaCore/external/QHY/sdk_Arm64_25.09.29/`.

- Camera IDs are strings (`char[32]`), not integers — use `std::optional<std::string>` for camera_id and `std::optional<int>` for camera_index.
- `GetQHYCCDSingleFrame()` blocks until the frame is ready; run it in a background thread and use an exposure status enum (Idle/Working/Success/Failed) to communicate results.
- Temperature control requires `ControlQHYCCDTemp()` to be called approximately every second; use a dedicated background thread started/stopped with the cooler.
- Guide direction convention differs from Alpaca: QHY uses EAST=0, NORTH=1, SOUTH=2, WEST=3 vs Alpaca North=0, South=1, East=2, West=3 — map explicitly.
- After changing readout mode, refresh chip info and reset ROI — sensor dimensions can change per mode.
- SDK global lifecycle (`InitQHYCCDResource` / `ReleaseQHYCCDResource`) is managed as a singleton in the wrapper; include `#define __CPP_MODE__ 1` before `#include <qhyccd.h>` in the wrapper `.cpp` only.
- Cameras require firmware files (`/lib/firmware/qhy/*.img` / `*.HEX`) in addition to udev rules. The udev rules call `fxload` to load firmware on plug-in, after which the device re-enumerates with a different USB product ID. Install firmware from `AlpacaCore/external/QHY/sdk_Arm64_25.09.29/lib/firmware/qhy/` to `/lib/firmware/qhy/`.
- The system `fxload` from apt does **not** support `-t fx3` (FX3-based cameras) and will exit 255 silently — always install the QHY SDK's own `fxload` binary from `sdk_Arm64_25.09.29/sbin/fxload` to `/sbin/fxload` instead.
- Re-enumeration in VMs: after `fxload` fires, the camera disconnects as `1618:c268` (Cypress WestBridge) and reconnects with its operational product ID. VMware and similar hypervisors will not automatically pass through the re-enumerated device unless the USB filter covers the entire QHYCCD vendor ID (`1618`). Test QHY cameras on bare metal or RPi rather than VMs where possible.

### SVBONY

Devices: Camera.

SDK location: `AlpacaCore/external/SVBONY/lib/armv8/`, headers under `external/SVBONY/include/`.

- **Control warm-up at connect (SV905C2 quirk)**: After `SVBOpenCamera`, `SVBSetControlValue(SVB_GAIN, ...)` returns `SVB_ERROR_GENERAL_ERROR` indefinitely on SV905C2 — regardless of value, regardless of `bAuto` flag, regardless of whether `SVBStartVideoCapture` is active, and `SVBRestoreDefaultParam` does not clear the state. The driver works around this by iterating every writable control reported by `SVBGetControlCaps` and writing each to its `default_value` during the connect path (after `SVBSetROIFormat` / `SVBSetOutputImageType`). Once any `SVBSetControlValue` call has landed, subsequent client gain writes succeed. Failures during the warm-up are tolerated and logged at DEBUG. Do not remove the warm-up loop in `set_connected` without re-running ConformU against an SV905C2 — the failure is silent until a client tries to set gain. Likely related to SDK readme entries `v1.13.1: Fixup ASCOM software to support SV905C2` and `v1.13.2: Optimize gain settings of SV905C2`.
- **Auto control writes**: `disable_auto_if_needed` reads the current value/auto flag and only writes back if currently auto, since some SVBONY models reject manual writes while auto is active with the same `SVB_ERROR_GENERAL_ERROR`.
- **`SVBSetControlValue` retry**: The wrapper retries up to 3 times with a 50 ms backoff specifically on `SVB_ERROR_GENERAL_ERROR` to absorb genuinely transient hardware-op faults; deterministic rejections still surface after the retries are exhausted.
- **Camera mode**: We use `SVB_MODE_NORMAL` (continuous video) and start/stop `SVBStartVideoCapture` per exposure. INDI's `indi-svbony` driver instead uses `SVB_MODE_TRIG_SOFT` with persistent video capture for stills — keep this in mind if a future SVBONY model needs trigger-mode behavior.
- **Bin/ROI quirks**: ROI width must be a multiple of 8 and height a multiple of 2 (SDK requirement). The driver aligns down for SDK calls while preserving the requested values for the Alpaca interface. ROI updates and `FrameSpeedMode` writes are deferred to `start_exposure` because some SDK control writes take ~1.1 s and would otherwise blow ASCOM client timing budgets.
- **`SVBRestoreDefaultParam`** is called immediately after `SVBOpenCamera` to clear any leftover state from a previous session, mirroring `indi-svbony`. Tolerate failure for older SDK builds that don't export the symbol.

### ToupTek

Devices: Camera, Focuser (AAF — Astro Auto Focuser).

SDK location: `AlpacaCore/external/ToupTek/toupcamsdk.20260128/` (shared between camera and focuser drivers).

- **Single SDK, two device types**: Both camera and focuser drivers go through `ToupTekSDKWrapper`. Cameras enumerate via `enumerate_cameras()`, focusers via `enumerate_focusers()` which filters `Toupcam_EnumV2` results by `TOUPCAM_FLAG_AUTOFOCUSER`. Same `Toupcam_Open` is used for both — the device-class is determined entirely by the capability flag.
- **AAF API convention** (`Toupcam_AAF(handle, action, value, *out)`):
  - **SET**: `Toupcam_AAF(h, AAF_SETxxx, value, nullptr)` — passes value in third arg.
  - **GET**: `Toupcam_AAF(h, AAF_GETxxx, 0, &out)` — third arg is unused, output via pointer.
  - **RANGE-of-GET**: `Toupcam_AAF(h, AAF_RANGEMAX, AAF_GETxxx, &out)` — queries the upper bound of a GET property by passing the GET action code as the third arg. Used to discover MaxStep and Backlash range at connect time. Same pattern works for `RANGEMIN` and `RANGEDEF`.
  - **HALT/SETZERO**: control actions, third arg is the new value (0 for halt; ticks for setzero/sync).
- **`AAF_GETSTEPSIZE` is mechanically meaningful only for specific focuser configurations** — the driver does not expose it as ASCOM `StepSize`. It throws `PropertyNotImplemented` per the focuser ASCOM contract.
- **Temperature units**: `AAF_GETTEMP` returns tenths of Celsius (e.g. `32` → `3.2 °C`). Divide by 10.0 before returning to ASCOM. INDI applies a 0.1 °C hysteresis when updating UI; we read on demand so the hysteresis is unnecessary on the driver side.
- **No temp-comp action**: The AAF action set has no temp-comp control. `TempCompAvailable` returns false; `set_temp_comp(true)` throws `NotImplemented` (not `DriverException`).
- **`Toupcam_get_FocusMotor` is deprecated** in the shipped SDK header. Do not use it for AAF focusers — use the `Toupcam_AAF` action interface instead. The non-deprecated `FocusMotor` API is for autofocus-equipped cameras (`TOUPCAM_FLAG_FOCUSMOTOR`), a different capability.

### SynScan (SkyWatcher)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/SynScan/`. No external SDK required — uses serial communication directly.

Connection types: Serial (USB serial) only. Default 9600 baud, 8N1. Protocol versions V3 (older) and V4 (current).

- Auto-detection scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for SynScan hand controllers, probes each port with a firmware version query, and connects to the first responding mount.
- **Pulse guiding**: SynScan V3/V4 protocol has no hardware pulse guide command. Driver implements software-timed variable-rate slew: issues a variable-rate axis slew at the guide rate, sleeps for the requested duration, then stops the axis and restores sidereal tracking. `IsPulseGuiding` tracks completion via time-based end time plus delay.
- **GEM pier-side DEC direction flip**: DEC motor direction is inverted when the mount's pointing state is 'W' (west), matching the physical axis reversal on German equatorial mounts. This affects pulse guide and MoveAxis DEC commands.
- **Position override accumulation**: Instead of reading back noisy mount positions after tiny guide pulses, the driver accumulates expected `rate × duration` deltas directly into the target coordinate frame. All consecutive pulse guide directions (N/S/E/W) operate in the same coordinate baseline, eliminating drift between reads.
- **RA tracking restoration**: The stop thread re-issues `set_tracking_mode()` after stopping an RA-axis pulse to counteract the variable-rate stop command killing sidereal tracking. Without this, the mount stops tracking after every RA pulse guide.
- ConformU 4.3.0 validated for **Sky-Watcher HEQ5 PRO** on Linux arm64 with 0 errors and 0 issues.

### iOptron

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/iOptron/RS-232_Command_Language2014V310.md`. No external SDK required — uses RS-232 serial communication directly.

Connection types: Serial (USB serial, 115200 baud default per v3.10 spec) and Network (WiFi TCP, default port 4030; varies by model — HEM27 uses 8899).

- Auto-detection scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for Prolific/FTDI/CP210x/Silicon Labs USB-serial adapters, probes each port with `:MountInfo#`, and connects to the first responding mount.
- **`:MountInfo#` quirk**: iOptron returns exactly 4 ASCII digit bytes with no `#` terminator. The protocol wrapper uses idle-timeout read mode (`require_hash_terminator=false`) for this command. Most other commands do terminate with `#`.
- **Model code table**: iOptron reassigned model codes between protocol v2 and v3 (e.g., code `0025` is HEM27 in v3, was CEM25 in v2). Use the INDI v3 driver's mapping, not the older Indigo-derived table.
- **SideOfPier**: The mount's `:GEP#` response includes a raw physical pier side value, but this does not match the ASCOM convention when tracking past the meridian. The driver computes SideOfPier from hour angle (LST − RA): `pierEast` (0) for HA ≥ 0, `pierWest` (1) for HA < 0. This matches the `DestinationSideOfPier` logic.
- **Sync**: Use the mount's `:CM#` command (after `:SRA#` and `:Sd#` to set target) to calibrate the mount's internal pointing model directly. Do NOT maintain driver-level sync offsets — they cause coordinate divergence during slews because the mount doesn't know about them. The `:CM#` approach keeps mount and driver in agreement.
- **Serial buffer flush**: Stale bytes from previous command responses can contaminate `:MS1#`/`:MS2#` slew responses (e.g., `"1111"` instead of `"1"`). The driver calls `flush_input()` (via `tcflush`/`PurgeComm`) before issuing slew commands.
- **Pulse guiding**: Uses native iOptron pulse guide commands (`:ZS#`, `:ZQ#`, `:ZE#`, `:ZC#` for N/S/E/W with duration in ms). Hardware-timed by the mount.
- **`:GEP#` response format**: sign + 8 RA digits + sign + 8 DEC digits + 1 side_of_pier digit + 1 pointing_state digit. No `#` terminator on some firmware versions — use idle-timeout read.
- ConformU 4.3.0 validated for **iOptron HEM27** on Linux arm64 with 0 errors and 0 issues.

### Celestron (NexStar)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Celestron/nexstar_protocol_reference.md` (combined reference — RS-232 serial, AUX bus, GPS, and AlpacaBridge driver notes). Original sources preserved in same directory. No external SDK required — uses RS-232 serial communication directly.

Connection types: Serial (RS-232 on hand control base) and Network (WiFi bridge adapters, default TCP port 2000).

- Communication: 9600 baud, no parity, one stop bit. All responses terminated with `#`.
- Position encoding: hexadecimal fraction of a revolution. Standard commands use 16-bit (4 hex digits, ~19.8 arcsec precision), precise commands use 24-bit (6 hex digits + "00" padding, ~0.08 arcsec precision). Commands: `E`/`e` for RA/Dec, `Z`/`z` for Alt/Az.
- GOTO: `R`/`r` for RA/Dec, `B`/`b` for Alt/Az. Sync: `S`/`s` (firmware v4.10+).
- Tracking modes: 0=Off, 1=Alt/Az, 2=EQ North, 3=EQ South.
- **CGE/Advanced GT quirk**: firmware versions 3.01–3.04 swap EQ North (1) and EQ South (2). This was corrected in later firmware. TODO: detect and handle this if needed.
- Model IDs from `"m"` command: 1=GPS, 3=i-Series, 4=i-Series SE, 5=CGE, 6=Advanced GT, 7=SLT, 9=CPC, 10=GT, 11=4/5 SE, 12=6/8 SE, 14=CGEM, 20=Advanced VX, 22=Evolution.
- Slew commands use pass-through (`P`) to motor controllers: device 16 = AZM/RA motor, device 17 = ALT/DEC motor. Fixed rates 1–9 (0 to stop), variable rates encoded as arcsec/sec × 4 in high/low bytes.
- Timeouts: NexStar spec says up to 3.5 seconds worst case for pass-through commands. Driver uses 5-second default.
- Time/Location: binary format (not ASCII). Timezone stored as hour offset (256-zone for negative). Location sign: 0=North/East, 1=South/West.
- **Home**: Mounts with hardware home switches (CGX, CGX-L, CGE Pro) use `MC_LEVEL_START` (0x0B) on both axes followed by polling `MC_LEVEL_DONE` (0x12). FindHome is asynchronous — send commands, return immediately, poll via Slewing/AtHome. Note: `MC_SEEK_INDEX` (0x19) / `MC_AT_INDEX` (0x18) are for PEC worm gear index (RA only), NOT home. See protocol reference for details.
- **Side of pier**: GEM mounts report pier side via the HC `p` command (`W` → pierWest, `E` → pierEast). This is a direct query — no hour angle inference needed.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via AUX bus pass-through. The firmware times the pulse internally — no sleep, encoder snapshotting, or sync calls required. Cross-axis is frozen at pre-pulse value during the guide window; active axis returns computed `baseline + (rate × duration)` as a one-shot correction to avoid ConformU tolerance failures at high declinations where cos(DEC) amplification causes geometric noise.
- **Adaptive RA slew offset**: Driver learns a running average of RA undershoot across slews and pre-biases subsequent slews to compensate for the CGX-L's no-tracking-during-goto behavior (matches INDI's `SlewOffsetRa` pattern).
- **Post-slew tracking restoration**: Re-issues the top-level `T` set-tracking-mode command rather than a per-axis variable-rate passthrough, keeping the HC's internal tracking state coherent with the LCD readout.
- **Site/time write skip when aligned**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped (log warn, return success) when the mount is aligned, matching INDI's UpdateLocation/UpdateTime pattern. Writing these after alignment corrupts the HC's pointing model. Preserves ConformU property round-trip tests.
- **Pier-safety gate**: Accepts either a successful `SyncToCoordinates` in the current driver session OR HC-reported alignment (`J` command). HC workflow: power on → Switch Position → Location → Last Alignment → "CGX-L Ready".
- ConformU 4.3.0 validated for **Celestron CGX-L** on Linux arm64 with 0 errors and 0 issues.
- The NexStar serial protocol is nearly identical to SynScan — both derive from the same Celestron protocol family. The driver implementation follows the same pattern but with separate namespace and branding.

### Bisque (Paramount / TheSkyX)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Bisque/` (INDI reference driver and TheSkyX scripting API docs). No external SDK required — communicates via JavaScript commands over TCP to TheSkyX Pro.

Connection types: TCP only. TheSkyX acts as middleware between the driver and the actual mount hardware.

- Communication is via JavaScript command snippets sent to the TheSkyX TCP scripting server (default port 3040).
- Commands are strings terminated with `#`. Responses are prefixed with `|No error. Error = 0.` on success, terminated with `#`.
- Special case: Handshake (`ConnectAndDoNotUnpark`/`IsConnected`) returns just `1` with no prefix.
- Slew is async: set `sky6RASCOMTele.Asynchronous = true`, call `SlewToRaDec`, poll `IsSlewComplete`.
- Pulse guiding uses `sky6DirectGuide.MoveTelescope(dRA, dDec)` with arcsecond displacement.
- Open loop motion for MoveAxis uses `DoCommand(9, 'direction|rate')` and `DoCommand(10, '')`.
- Park uses `ParkAndDoNotDisconnect()` to keep TCP connection alive (not `Park()` which disconnects).
- Pier side is read-only via `DoCommand(11, 'Pier Side')` — returns 1 for west of pier, else east.
- Find Home uses `FindHome()` with a 60-second timeout.
- Slew speed presets: 9 rates (1x, 2x, 4x, 8x, 32x, 64x, 128x, 256x, 512x sidereal).

### Gemini (Automatic Astro Focuser Pro)

Devices: Focuser.

Protocol: MyFocuserPro2 serial protocol (Arduino-based). Reference docs in `AlpacaCore/external/Losmandy/` (Gemini L4 command set). No external SDK required.

Connection types: USB serial (CH340/CH341 adapters) only. No WiFi support.

- **DTR/HUPCL quirk**: CH340 USB-serial adapters assert DTR on port open, which resets the Arduino/ESP32 MCU. During auto-detect probe, the driver clears HUPCL before closing the port so DTR stays high. On the subsequent connect, the MCU does not reset again. Do not remove this HUPCL handling — it prevents a ~4 second double-boot penalty and possible connection failure.
- Auto-detection scans `/dev/serial/by-id/` for CH340/CH341 chips (USB vendor `1a86`), falls back to `/dev/ttyUSB0` through `/dev/ttyUSB9`.
- Connection uses 3 retry attempts with staggered wait times (100ms, 2s, 1s) — worst case ~9.1 seconds, designed to stay under the ASCOM client 10-second timeout.
- Motor speed is forced to fast (`:1502#`) on connect so moves complete within ConformU's 60-second timeout.
- Temperature is forced to Celsius (`:16#`) on connect — do not assume the focuser's default unit.
- `get_step_size()` throws `PropertyNotImplemented` — the MyFocuserPro2 protocol does not expose step size in microns; it varies by mechanical configuration.
- Movement commands (`:05<pos>#`) are fire-and-forget (blind). Poll `is_moving()` (`:01#`) to detect completion.
- Some commands are blind (no response). Use `send_command_blind_locked()` for these to avoid blocking on a timeout waiting for data that will never arrive.
- Default baud rate is 9600 (8N1). Configurable: 9600, 19200, 38400, 57600, 115200.
- ConformU validated (v4.2.1) on Debian 13 x64 and ARM64. Results in `AlpacaCore/conformu/Gemini/`.

### WeeWX

Devices: ObservingConditions.

No external SDK — reads weather data from a local WeeWX weather station instance.

## General Notes

- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- ConformU logs live under `AlpacaCore/conformu/`.
- Filter wheel DeviceState should only include operational fields (e.g., `Position`); omit `Connected` for ConformU compatibility.
- Filter wheel Names must be non-empty; default to `"Filter 1..N"` and allow setting names/offsets while disconnected.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add desktop GUI frameworks (Qt, GTK, wxWidgets, etc.). The web UI in `AlpacaHTTP/web/` is the only user interface.
