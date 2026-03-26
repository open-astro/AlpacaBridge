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

## Target Architectures

- All drivers must build and run on both **arm64** (ARMv8, e.g. Raspberry Pi 4/5) and **amd64** (x86_64).
- When writing driver code, account for architecture differences:
  - **Endianness**: both targets are little-endian today, but do not assume byte order — use explicit serialization when packing/unpacking wire data.
  - **Alignment**: arm64 is stricter — do not cast arbitrary byte buffers to struct pointers; use `memcpy` or per-field reads.
  - **Data type sizes**: use fixed-width types (`int32_t`, `uint16_t`, etc.) for hardware registers, protocol fields, and SDK structs. Do not assume `int`, `long`, or pointer sizes.
  - **Vendor SDK libraries**: must be provided for both architectures under `AlpacaCore/external/<VENDOR>/lib/linux/armv8/` and `lib/linux/x64/`. If a vendor only ships one architecture, document the gap and guard the build with an architecture check in CMake.
  - **Floating-point**: avoid `long double` (80-bit on x64, 128-bit on arm64). Use `double` for all floating-point protocol values.
- CMake, `debian/rules`, `build_and_run.sh`, and `install_alpaca_service.sh` all detect the host architecture at build/install time — keep them in sync when adding architecture-dependent paths.
- Test on both architectures before declaring a driver ConformU-validated. ConformU results are stored per-architecture in `AlpacaCore/conformu/`.

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

When a vendor SDK provides shared libraries (`.so`) that are needed at runtime — either by AlpacaBridge itself or by companion projects (e.g. SmartGuider uses `libASICamera2.so` via the `zwoasi` Python package) — they must be included in the repository and installed by all packaging/deployment scripts.

- Store both static (`.a`) and shared (`.so`) libraries under `AlpacaCore/external/<VENDOR>/` in the architecture-appropriate subdirectory (e.g. `lib/linux/armv8/`, `lib/linux/x64/`).
- Ensure `.gitignore` allowlist entries (`!external/<VENDOR>/**`) cover `.so` files as well as `.a` files.
- Update **all three** install/packaging paths to install the `.so` to a system library path and run `ldconfig`:
  1. **`debian/rules`** — copy `.so*` to `$(STAGING)/usr/lib/alpacabridge/` in `override_dh_auto_install`. This ensures the `.deb` package ships the shared library.
  2. **`build_and_run.sh`** — detect architecture, copy `.so*` to `/usr/local/lib/`, run `ldconfig`. Add inside the udev rules block alongside existing QHY/ZWO install logic.
  3. **`install_alpaca_service.sh`** — same as `build_and_run.sh`, inside the `install_udev_rules()` function.
- Keep the install logic in `build_and_run.sh` and `install_alpaca_service.sh` in sync — they must install the same set of vendor libraries.
- When a vendor releases a new SDK version, update the `.so` files in `external/` and bump symlink targets (e.g. `libASICamera2.so.1.41` → `libASICamera2.so.1.42`).

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

## Logging, Threading, and Errors

- Use AlpacaCore logging sink flow; do not use ad-hoc stdout/stderr logging in runtime paths.
- Avoid global mutable state; protect shared state with mutexes.
- Use `AlpacaException` for error paths; AlpacaHTTP maps exceptions to Alpaca error responses.
- AlpacaHTTP must return Alpaca-style JSON envelopes and stable error mapping behavior.

## Units and Behavior Conventions

- Exposure: seconds
- Angles: degrees
- RA: hours
- Dec: degrees
- Pixel size: microns
- Time: UTC with `std::chrono`

## Vendor-Specific Notes

### ZWO

Devices: Camera, FilterWheel, Focuser (EAF), Rotator, Switch (dew heater), Telescope (AM mount).

SDK locations: `AlpacaCore/external/ZWO/ASI_Camera_SDK/`, `EAF/`, `EFW/`, `CAA/`, `AM/`.

- ROI sizing rules: width must be a multiple of 8 and height a multiple of 2 after binning. Keep requested sizes for Alpaca, align effective sizes down for SDK calls, and pad outputs if needed.
- Dew heater is exposed as an Alpaca Switch device (not a camera action) and is camera-dependent.
- ST4 pulse guiding should be enabled only when the SDK reports `has_st4_port`.
- PulseGuide: do not apply permanent RA/Dec offsets based on expected guide motion. If synthetic offsets are needed, keep them temporary and clear after the pulse completes to avoid double-counting mount motion.
- The ZWO and QHY SDKs both statically link libusb, causing duplicate symbol issues. The ZWO vendor `CMakeLists.txt` handles this — do not link both vendor static libs into the same binary without resolving the conflict.

### QHY

Devices: Camera.

SDK locations: `AlpacaCore/external/QHY/sdk_Arm64_25.09.29/`, `sdk_linux64_25.09.29/`.

- Camera IDs are strings (`char[32]`), not integers — use `std::optional<std::string>` for camera_id and `std::optional<int>` for camera_index.
- `GetQHYCCDSingleFrame()` blocks until the frame is ready; run it in a background thread and use an exposure status enum (Idle/Working/Success/Failed) to communicate results.
- Temperature control requires `ControlQHYCCDTemp()` to be called approximately every second; use a dedicated background thread started/stopped with the cooler.
- Guide direction convention differs from Alpaca: QHY uses EAST=0, NORTH=1, SOUTH=2, WEST=3 vs Alpaca North=0, South=1, East=2, West=3 — map explicitly.
- After changing readout mode, refresh chip info and reset ROI — sensor dimensions can change per mode.
- SDK global lifecycle (`InitQHYCCDResource` / `ReleaseQHYCCDResource`) is managed as a singleton in the wrapper; include `#define __CPP_MODE__ 1` before `#include <qhyccd.h>` in the wrapper `.cpp` only.
- Cameras require firmware files (`/lib/firmware/qhy/*.img` / `*.HEX`) in addition to udev rules. The udev rules call `fxload` to load firmware on plug-in, after which the device re-enumerates with a different USB product ID. Install firmware from `AlpacaCore/external/QHY/sdk_<arch>_*/lib/firmware/qhy/` to `/lib/firmware/qhy/` using the architecture-matching SDK directory.
- The system `fxload` from apt does **not** support `-t fx3` (FX3-based cameras) and will exit 255 silently — always install the QHY SDK's own `fxload` binary from `sdk_<arch>_*/sbin/fxload` to `/sbin/fxload` instead.
- Re-enumeration in VMs: after `fxload` fires, the camera disconnects as `1618:c268` (Cypress WestBridge) and reconnects with its operational product ID. VMware and similar hypervisors will not automatically pass through the re-enumerated device unless the USB filter covers the entire QHYCCD vendor ID (`1618`). Test QHY cameras on bare metal or RPi rather than VMs where possible.

### SynScan (SkyWatcher)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/SynScan/`. No external SDK required — uses serial communication directly.

### iOptron

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/iOptron/`. No external SDK required — uses RS-232 serial communication directly.

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
