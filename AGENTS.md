# Agent Instructions (Codex Source of Truth)

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
- Call flow is always:
  - `AlpacaHTTP -> AlpacaCore driver -> vendor implementation`.

## Language, Style, and Safety

- C++20 preferred, RAII, small focused functions.
- Use `#pragma once` in headers.
- Prefer `enum class`, `std::chrono`, `std::string_view` where appropriate.
- No `using namespace std;` in headers.
- Core/driver layers should avoid heavy framework dependencies.
- License headers must remain SSPL v1 and unmodified in all source files.

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
- QHY-specific driver notes:
  - Camera IDs are strings (`char[32]`), not integers — use `std::optional<std::string>` for camera_id and `std::optional<int>` for camera_index.
  - `GetQHYCCDSingleFrame()` blocks until the frame is ready; run it in a background thread and use an exposure status enum (Idle/Working/Success/Failed) to communicate results.
  - Temperature control requires `ControlQHYCCDTemp()` to be called approximately every second; use a dedicated background thread started/stopped with the cooler.
  - Guide direction convention differs from Alpaca: QHY uses EAST=0, NORTH=1, SOUTH=2, WEST=3 vs Alpaca North=0, South=1, East=2, West=3 — map explicitly.
  - After changing readout mode, refresh chip info and reset ROI — sensor dimensions can change per mode.
  - QHY SDK global lifecycle (`InitQHYCCDResource` / `ReleaseQHYCCDResource`) is managed as a singleton in the wrapper; include `#define __CPP_MODE__ 1` before `#include <qhyccd.h>` in the wrapper `.cpp` only.

## CMake and Vendor Integration

- Guard each vendor behind explicit build options.
- When adding a vendor in `AlpacaCore/CMakeLists.txt`, always update:
  1. `option(ALPACACORE_ENABLE_<VENDOR> ...)`
  2. `ALPACACORE_ENABLE_ALL_VENDORS` logic (only if implemented)
  3. conditional `add_subdirectory(src/vendors/<vendor>)` + link
  4. install rules for vendor target
- If vendor libs are discovered by pkg-config, prefer imported targets (example: `PkgConfig::LIBUSB`) so dependent test binaries get correct link paths.
- When adding a new vendor SDK under `AlpacaCore/external/<vendor>/`, add an allowlist entry to `AlpacaCore/.gitignore` so the SDK binaries (`.a`, `.so`, `.dll`, firmware files, etc.) are not blocked by the global compiled-file ignore rules. Follow the existing pattern: `!external/<VENDOR>/**`.

## AlpacaHTTP Integration Checklist (Required for New Vendor/Device Types)

When adding a new vendor/device type in AlpacaCore, also update AlpacaHTTP:

- Router registration in `Router::register_device_from_config`.
- Config sanitization fields.
- Web UI vendor dropdown and vendor-specific form fields.
- Any related validation logic in frontend JS.
- Build-flag propagation/compile definitions when needed.
- Routing/config tests.

Vendor registration alone is not enough for HTTP/UI visibility.

## Testing Requirements

- Non-trivial code must have unit tests under `AlpacaCore/tests/`.
- Build driver targets and test targets together.
- Tests should be runnable via:
  - `run_all_tests.cmd`
  - `run_all_tests.sh`
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

## Project-Specific Notes

- ZWO ROI sizing rules: width must be a multiple of 8 and height a multiple of 2 after binning. Keep requested sizes for Alpaca, align effective sizes down for SDK calls, and pad outputs if needed.
- ZWO dew heater is exposed as an Alpaca Switch device (not a camera action) and is camera-dependent.
- ST4 pulse guiding should be enabled only when the SDK reports `has_st4_port`.
- ZWO PulseGuide: do not apply permanent RA/Dec offsets based on expected guide motion. If synthetic offsets are needed, keep them temporary and clear after the pulse completes to avoid double-counting mount motion.
- On macOS, the ZWO SDK links against libusb even if the camera appears in System Report.
- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- QHY cameras require firmware files (`/lib/firmware/qhy/*.img` / `*.HEX`) in addition to udev rules. The udev rules call `fxload` to load firmware on plug-in, after which the device re-enumerates with a different USB product ID. Install firmware from `AlpacaCore/external/QHY/sdk_<arch>_*/lib/firmware/qhy/` to `/lib/firmware/qhy/` using the architecture-matching SDK directory. Verify `fxload` is at `/sbin/fxload` (the path hard-coded in the rules).
- QHY re-enumeration in VMs: after `fxload` fires, the camera disconnects as `1618:c268` (Cypress WestBridge) and reconnects with its operational product ID. VMware and similar hypervisors will not automatically pass through the re-enumerated device unless the USB filter covers the entire QHYCCD vendor ID (`1618`). Test QHY cameras on bare metal or RPi rather than VMs where possible.
- ConformU logs live under `AlpacaCore/conformu/`; Windows logs are prefixed with `W-` for comparison.
- Filter wheel DeviceState should only include operational fields (e.g., `Position`); omit `Connected` for ConformU compatibility.
- Filter wheel Names must be non-empty; default to `"Filter 1..N"` and allow setting names/offsets while disconnected.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add GUI/branding/unrelated framework layers.
