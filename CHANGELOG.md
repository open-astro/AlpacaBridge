# AlpacaBridge Changelog

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

## [1.0.3] - 2026-04-21

### Added
- **Player One Camera Driver** (AlpacaCore)
  - New vendor driver for Player One astronomy cameras with ASCOM Alpaca Camera API (ICameraV3) support.
  - SDK wrapper singleton (`PlayerOneSDKWrapper`) over Player One Camera SDK v3.10.0 managing camera enumeration, lifecycle, and the generic `POAConfig` table (gain, offset, exposure, cooler, ST4 pulse guide, temperature, etc.).
  - Exposure via single-frame software trigger with background thread, `POAImageReady` polling, abort support, and a 15 s deadline safety margin.
  - Capability-gated cooler control (`POA_COOLER` / `POA_TARGET_TEMP` / `POA_COOLER_POWER`) so uncooled cameras accurately report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`.
  - ST4 pulse guiding gated on `isHasST4Port`; pulse duration timed by the driver via `POA_GUIDE_NORTH/SOUTH/EAST/WEST` bool toggles.
  - RAW8 / RAW16 / RGB24 / MONO8 image format support with format-aware `ImageArray` builders (RGB24 transposes the SDK's B,G,R pixel order to R,G,B and reports rank 3).
  - SDK requires `width % 4 == 0` and `height % 2 == 0`; driver accepts any user-provided ROI and internally aligns the SDK call DOWN while returning the `ImageArray` at the user's exact `NumX × NumY` (trailing row/col zero-padded).
  - Camera binding by index (`cameraIndex`) from the Player One SDK enumeration.
  - Architecture-aware SDK selection: `lib/x64/` (Linux x86_64) and `lib/arm64/` (Linux ARM64).
  - ConformU validated for **Player One Ceres 462M** on Linux arm64 with 0 errors and 0 issues; x64 validation pending.
- **Player One Device Support** (AlpacaHTTP)
  - Router registration and configuration support for Player One camera devices (`cameraIndex` binding).
  - Web UI: Player One vendor selection and camera-index configuration field.
- **Player One SDK** (AlpacaBridge)
  - Player One Camera SDK v3.10.0 libraries included under `AlpacaCore/external/PlayerOne/`; `.gitignore` allowlist added.
  - `libPlayerOneCamera.so` installed to `/usr/lib/alpacabridge/` (Debian package) and `/usr/local/lib/` (build scripts) so companion tools (e.g. SmartGuider) can `dlopen` the Player One SDK at runtime.
  - `99-player_one_astronomy.rules` udev rule installed for Player One USB devices.
- **Player One Unit Tests** (AlpacaCore)
  - 6 test cases covering defaults, device metadata, disconnected throws, disconnected state, unsupported actions, and sub-exposure rejection.

### Changed
- **External SDK directory**: renamed `AlpacaCore/external/Player One/` (space) → `AlpacaCore/external/PlayerOne/` (no space) so `debian/rules` Makefile variables and shell install scripts don't break on the embedded whitespace.
- **Supported Drivers Documentation**: added Player One section between QHY and SVBONY with Ceres 462M entry, ConformU link, and driver notes (SDK version, tested model, cooling gating, dew-heater not-wired status, pulse guiding mechanism).

## [1.0.2] - 2026-04-18

### Added
- **SVBONY Camera Driver** (AlpacaCore)
  - New vendor driver for SVBONY cameras with full ASCOM Alpaca Camera API (ICameraV3) support.
  - SDK wrapper singleton (`SVBSDKWrapper`) managing camera enumeration, lifecycle, and shared SDK init/release.
  - Exposure via video capture mode (start capture → `SVBGetVideoData` → stop) with background thread.
  - Gain, offset, ROI, binning, temperature readout, and ST-4 pulse guide support (`SVBPulseGuide`).
  - Camera binding by index only (SVBONY SDK does not support camera ID lookup).
  - Architecture-aware SDK selection: `lib/x64/` (Linux x86_64) and `lib/armv8/` (Linux ARM64).
- **SVBONY Device Support** (AlpacaHTTP)
  - Router registration and configuration support for SVBONY camera devices (`cameraIndex` binding).
  - Web UI: SVBONY vendor selection and camera index configuration field.
- **SVBONY SDK** (AlpacaBridge)
  - SVBONY SDK libraries included under `AlpacaCore/external/SVBONY/`; `.gitignore` allowlist added.
- **ToupTek Camera Driver** (AlpacaCore)
  - New vendor driver for ToupTek cameras with ASCOM Alpaca Camera API (ICameraV3) support.
  - SDK wrapper (`ToupTekSDKWrapper`) over `toupcamsdk.20260128` managing camera enumeration, lifecycle, and shared SDK init/release.
  - Exposure via ToupTek pull-mode still capture with configurable gain, offset, ROI, and binning; temperature readout and TEC cooling gated on `TOUPCAM_FLAG_TEC` / `TOUPCAM_FLAG_TEC_ONOFF` so uncooled guide cameras expose an accurate capability set.
  - Camera binding by index (`cameraIndex`) from the ToupTek SDK enumeration.
  - Architecture-aware SDK selection: `linux/x64/` (Linux x86_64) and `linux/arm64/glibc/` (Linux ARM64).
  - ConformU validated for **ToupTek GPCMOS01200KPF** on Linux x86_64 with 0 errors and 0 issues; other ToupTek models are expected to work, but the driver's dew-heater support is not yet wired (no cooled ToupTek camera available for testing).
- **ToupTek Device Support** (AlpacaHTTP)
  - Router registration and configuration support for ToupTek camera devices (`cameraIndex` binding).
  - Web UI: ToupTek vendor selection and camera-index configuration field.
  - New routing test covering a ToupTek camera configure/list/remove round-trip.
- **ToupTek SDK** (AlpacaBridge)
  - ToupTek SDK libraries included under `AlpacaCore/external/ToupTek/`; `.gitignore` allowlist added.
  - `libtoupcam.so` installed to `/usr/lib/alpacabridge/` (Debian package) and `/usr/local/lib/` (build scripts), and registered with the dynamic linker via `/etc/ld.so.conf.d/alpacabridge.conf` so companion tools (e.g. SmartGuider) can load the ToupTek SDK at runtime.
  - `99-toupcam.rules` udev rule installed for ToupTek USB devices.
- **Expanded Unit Tests** (AlpacaCore)
  - SVBONY camera: 6 test cases (defaults, metadata, disconnected throws, disconnected state, unsupported actions, sub-exposure).
  - QHY camera: expanded from 1 to 6 test cases.
  - ZWO camera: expanded from 1 to 6 test cases.
  - ZWO focuser: expanded from 2 to 5 test cases (metadata, device number assignment, unique IDs).
  - ZWO filter wheel: expanded from 2 to 5 test cases (metadata, device number assignment, unique IDs).
  - ZWO rotator: expanded from 2 to 5 test cases (metadata, device number assignment, unique IDs).
  - ZWO switch: expanded from 3 to 5 test cases (metadata, unsupported actions).
  - SynScan telescope: expanded from 3 to 5 test cases (metadata, disconnected behavior).
  - iOptron telescope: expanded from 4 to 5 test cases (metadata).
  - WeeWX observing conditions: expanded from 1 to 4 test cases (defaults, metadata, unsupported actions).
  - Total: 69 test cases, 484 assertions across all vendor drivers.
- **AGENTS.md**: added required test case checklist for new vendor device drivers and CMake integration steps.
- **Bisque Paramount Telescope Driver** (AlpacaCore / AlpacaHTTP) — *in progress*
  - Initial Bisque / Paramount driver over the TheSkyX TCP protocol; vendor plumbing and web UI configuration panel added.
  - Hidden from the web UI vendor dropdown pending ConformU validation.
- **Celestron NexStar CGX-L Telescope Driver** (AlpacaCore / AlpacaHTTP) — *partial, in progress*
  - Initial Celestron NexStar driver (CGX-L target) with serial/network connection support; vendor plumbing and web UI configuration panel added.
  - Hidden from the web UI vendor dropdown pending completion and ConformU validation.

### Changed
- **Documentation** (AlpacaBridge)
  - README rewritten around installation via the [apt.openastro.net](https://apt.openastro.net) APT repository now that packaged releases are public.
  - Development workflow (build scripts, CMake flags, source-install, custom-driver guidance) moved to a new `DEVELOPMENT.md`.
  - Added Wiki link to README.
- **Web UI** (AlpacaHTTP)
  - Celestron and Bisque vendor options temporarily hidden in the device configuration dropdown until their drivers ship.
- **AGENTS.md**: rewrote the "Vendor SDK Shared Library Packaging" section with a mandatory 6-step checklist (SDK placement, both-arch coverage, `.gitignore` allowlist with `git check-ignore` verification, `debian/rules`, `build_and_run.sh`, `install_alpaca_service.sh`) plus a dynamic-linker registration explainer, so future camera vendors (PlayerOne next) cannot ship with the `.so` missing from apt/package/source-install paths.

### Fixed
- SynScan Telescope: `SideOfPier` comment corrected — OTA east of pier observes west, not east.
- **SVBONY SV905C2 gain writes** (AlpacaCore)
  - Gain writes on the SV905C2 were rejected by the SDK unless writable controls were warmed up at connect; added a control warm-up pass on connect to unlock gain writes.
  - Added an exposure watchdog so `CameraState` recovers when the SVBONY SDK hangs mid-exposure instead of leaving the camera stuck in `Exposing`.
- **AlpacaHTTP build**: added the missing Bisque compile definition in `AlpacaHTTP/CMakeLists.txt` so the in-progress Bisque driver builds cleanly.
- **ToupTek Defaults test** (AlpacaCore): loosened the `get_name()` assertion in `test_touptek_camera.cpp` from a substring match on `"ToupTek"` to a non-empty check, since the SDK returns the model-specific `displayname` (e.g. `"GPCMOS01200KPF"`) whenever a physical camera is attached.

### Removed
- `build_and_run.cmd` (Windows-only build script; workspace has been Linux-only since 1.0.0).

## [1.0.1] - 2026-03-27

### Added
- SynScan Telescope: Sky-Watcher HEQ5 PRO ConformU validation (x64 and ARM64).

### Changed
- SynScan Telescope: Device name changed from "SynScan Mount"/"SynScan Telescope" to "SynScan V3/V4 Telescope" across the driver and web portal to clarify hand controller compatibility.
- SynScan `SideOfPier` mapping: swapped `pierEast`/`pierWest` values to match ASCOM convention.
- SUPPORTED-DRIVERS.md: Added Sky-Watcher HEQ5 PRO to SynScan mount table.

## [1.0.0] - 2026-03-26

### Added
- Gemini Automatic Astro Focuser Pro driver (AlpacaCore/AlpacaHTTP)
  - New vendor driver for Gemini/MyFocuserPro2-compatible focusers using the MyFP2 serial protocol. Supports serial (USB) and auto-detection of CH340/CH341 USB-serial adapters.
  - Auto-detection scans `/dev/serial/by-id/` for CH340/CH341 devices and probes with firmware handshake. CH340 DTR reset handling clears HUPCL to prevent double MCU reset.
  - Async connect with polling to avoid ASCOM Alpaca client timeouts (NINA compatibility).
  - ConformU compliant: out-of-range moves clamp to 0/MaxStep; motor speed set to fast on connect.
  - AlpacaHTTP web UI: vendor dropdown, connection type selector (Auto-detect/Serial).
  - Catch2 unit tests: 7 test cases, 42 assertions.
- Unit tests for iOptron telescope and ZWO Dew Heater Switch drivers.
- Troubleshooting docs: serial port connection failures.
- Refreshed ConformU test reports for all drivers on Linux x64 and ARM64.
- Debian packaging (`debian/`): service file, maintainer scripts.
- ZWO ASI Camera shared library packaging for Debian (`.deb` ships `libASICamera2.so`).
- `ld.so.conf.d` registration so vendor shared libraries are discoverable system-wide.
- Favicon using OpenAstro logo.

### Fixed
- iOptron Telescope: `SiteLatitude` write timing on Wi-Fi, `SlewToCoordinatesAsync` async dispatch, tertiary AxisRate empty range.
- ZWO CAA Rotator: mechanical position when Reverse is enabled.
- ZWO Telescope (AM3): `SideOfPier` hour-angle derivation, `SyncToCoordinates` retry on moving mount, `PulseGuide` accuracy and timing, Wi-Fi stability, TCP_NODELAY for Nagle latency.
- WeeWX ObservingConditions: consistent `PropertyNotImplemented` for missing sensors.
- Gemini Focuser: amd64 move timeout and disconnect issues.
- Missing web UI assets in Debian package.

### Changed
- Platform support: Linux only (Debian 13 Trixie, NUC x64, Raspberry Pi 4/5 ARM64). Removed Windows and macOS from build, docs, CMake, and source.
- iOptron Telescope name is now always "iOptron Telescope" (removed `:MountInfo#` querying).
- ZWO EAF Focuser `get_step_size()` throws `PropertyNotImplemented` instead of returning 0.0.
- Driver versions: SynScan and ZWO telescope drivers now report 1.0.0.
- SUPPORTED-DRIVERS.md moved to repo root; tables and links updated for Linux-only.
- ZWO ASI Camera SDK moved to `external/ZWO/ASI_Camera_SDK`.
- `build_and_run.sh`: optional `ALPACA_ADD_DIALOUT` env for serial/USB group access.
- Debian `postinst` adds `dialout` and `input` groups for USB device access.
- Version set to 1.0.0.

### Removed
- Windows/macOS scripts, ConformU reports, and platform-specific code paths.

## [0.13.0] - 2026-03-12

### Added
- **QHY Camera Driver** (AlpacaCore)
  - Complete QHY camera driver implementation with full ASCOM Alpaca Camera API (ICameraV3) support.
  - SDK wrapper singleton (`qhy_sdk_wrapper`) managing camera enumeration lifecycle and shared SDK init/release.
  - Full exposure control with background exposure thread, abort, and stop support.
  - Temperature and cooler control with PID management (~1 s polling loop); `CanGetCoolerPower` disabled to avoid SDK stalls on CURPWM queries.
  - Binning support (1×1, 2×2, 3×3, 4×4) with per-camera capability checks.
  - ROI (Region of Interest) configuration.
  - Readout mode enumeration (7 modes on QHY268C: Photographic DSO, High Gain, Extended Fullwell, etc.).
  - ST-4 pulse guide support with Alpaca-to-QHY direction mapping.
  - Gain (0–142) and offset (0–255) control.
  - Color camera detection with correct Bayer pattern identification (RGGB, GRBG, BGRG, etc.); 16-bit image array retrieval.
  - Camera binding via `cameraId` or `cameraIndex` configuration.
  - Architecture-aware SDK selection at build time: `sdk_Arm64_25.09.29` (Linux ARM64) and `sdk_linux64_25.09.29` (Linux x86_64); links against `libqhyccd.so` (shared) to avoid pre-`main()` SDK constructor crashes.
  - ConformU validated for **QHY268C** (6280×4210, 16-bit, RGGB) on Linux ARM64 with 0 errors and 0 issues.
- **QHY Device Support** (AlpacaHTTP)
  - Router registration and configuration support for QHY camera devices (`cameraId` / `cameraIndex` binding).
  - Web UI: QHY vendor selection and camera index/ID configuration fields.
- **QHY SDK & Firmware** (AlpacaBridge)
  - QHY SDK libraries included in repository under `AlpacaCore/external/QHY/` (ARM64 and x86_64); `.gitignore` allowlist added.
  - Build and install scripts deploy QHY firmware to `/lib/firmware/qhy/`, install QHY's `fxload` to `/sbin/fxload` (required for FX3-based cameras), install `libqhyccd.so*` to `/usr/local/lib/`, and run `ldconfig`.
  - Udev rule deduplication: installs a single `85-qhyccd.rules` from the architecture-matched SDK (QHY ships 3 copies).

### Fixed
- **QHY Color Detection** (AlpacaCore)
  - `IsQHYCCDControlAvailable(handle, CAM_IS_COLOR)` returns the Bayer ID (1–4) for color cameras, not `QHYCCD_SUCCESS` (0). The old check (`ret == QHYCCD_SUCCESS`) incorrectly flagged all cameras—including the QHY268C—as monochrome. Fixed by checking `ret != QHYCCD_ERROR` and storing the raw Bayer ID to derive accurate Alpaca `BayerOffsetX`/`BayerOffsetY` values.
- **QHY Mutex Deadlocks** (AlpacaCore)
  - Temperature-control thread held `mutex_` on each iteration; calling `join()` while `mutex_` was still locked caused an ABBA deadlock that froze NINA and blocked camera enumeration. Fixed by signaling stop, moving the thread handle under the lock, then joining outside the lock in both `set_connected_impl()` and `set_cooler_on()`.
- **QHY SDK Scan Guard** (AlpacaCore)
  - `ScanQHYCCD()` can return `QHYCCD_ERROR` (0xFFFFFFFF = 4294967295) on failure; calling `reserve()` with that value caused `std::bad_alloc`. Added a guard to treat this return value as zero cameras found.

### Changed
- **Supported Drivers Documentation** (AlpacaCore)
  - Added QHY Camera Drivers section to SUPPORTED-DRIVERS.md with QHY268C entry, ConformU link, and driver notes (SDK version, color detection behavior, firmware/udev install requirements, Linux ARM64 and x86_64 platform support).

## [0.12.1] - 2026-02-25

### Fixed
- **Linux x64 Build** (AlpacaCore)
  - Added ZWO CAA SDK Linux x64 library (`external/ZWO/CAA/lib/x64/libCAA.a`) so the ZWO vendor build completes on Linux x64. The vendored CAA SDK previously only included armv6/armv7/armv8, mac, and Windows; Linux x64 was missing and caused the build to fail.

### Changed
- **ZWO Telescope (AM5N) Driver** (AlpacaCore)
  - AM5N driver is working and tested with **USB** and **WiFi** connections; ConformU passes with 0 issues/0 errors.
  - PulseGuide and slew-adjustment fixes: pending slew adjustment now uses a fresh mount query (no stale cache); longitude no longer restored mid-slew after GOTO retry; sync slew path clears pending flag to avoid double adjustment.
  - WiFi timing: extended equatorial cache TTL for RA/Dec reads (5 s) so FAST response target is met over high-latency links; PulseGuide no longer performs a mount query for debug logging before returning, improving STANDARD timing.
  - Platform coverage: AM5 / AM5N driver has been exercised on Linux ARMv8 (e.g., Raspberry Pi 5) in addition to Windows 11 (x64), macOS (arm64), and Linux x64; behavior and ConformU results match desktop platforms.
- **Supported Drivers Documentation** (AlpacaCore)
  - SUPPORTED-DRIVERS.md updated: ZWO AM5N tested and working over USB and WiFi (macOS arm64).
  - Added Linux x64 platform support: ZWO AM5N telescope verified on Linux x64; ZWO Telescope Driver Notes updated with Linux x64 in Verified OS/Arch.
  - Added Linux Notes section with Linux x64 testing and serial port access: user must run `sudo usermod -aG dialout $USER` and log out/back in for USB/serial device access.
  - Updated ZWO telescope entry to document AM5 / AM5N row and Linux ARMv8 (e.g., Raspberry Pi 5) verification, plus Linux ARM testing notes consistent with other ZWO drivers.

## [0.12.0] - 2026-02-21

### Added
- **ZWO Telescope (ASI Mount) Driver** (AlpacaCore)
  - ZWO mount telescope driver using ZWO Mount Serial Communication Protocol.
  - Protocol wrapper (`zwo_mount_protocol_wrapper`) and driver implementation for serial (USB/Bluetooth) and optional network connection.
  - Protocol reference docs under `external/ZWO/AM/` (v1.8, v2.0, v2.1, final, extended, undocumented).
  - Unit tests for ZWO telescope driver; ZWO mount build integrated in vendor CMakeLists and tests CMakeLists.
- **ZWO Telescope Device Support** (AlpacaHTTP)
  - Router registration and configuration support for ZWO telescope (mount) devices.
  - Web UI: ZWO Mount (Telescope) configuration (connection type, serial port/path, baud rate, network host/port, aperture diameter).
  - Routing tests updated for telescope/mount device type and ZWO telescope registration.

### Changed
- **Supported Drivers Documentation** (AlpacaCore)
  - Documented ZWO AM5N in Telescope drivers table; ConformU validation (macOS arm64); driver notes (protocol, connection, tested firmware 1.8.8, USB/serial only—Bluetooth not tested).
  - ZWO AM5N known issue: firmware issue—guiding on its own can be sporadic; guiding via ST4 cable to the guide camera should still be fine.

## [0.11.2] - 2026-02-20

### Fixed
- **Discovery service** (AlpacaHTTP)
  - Discovery loop now uses `select()` with a 200 ms timeout so `stop()` can terminate promptly on all platforms instead of blocking on `recvfrom()`. Removed socket close from `stop()` so the discovery thread exits cleanly; added error handling for `select()` and `recvfrom()` (interrupted / would-block continue; other errors logged and break).

## [0.11.1] - 2026-02-19

### Changed
- **AlpacaCore tests** (AlpacaCore)
  - Tests now require Catch2 only (doctest fallback removed). CMake supports both `Catch2::Catch2WithMain` and `Catch2::Catch2Main` for Catch2 v2/v3 compatibility.
  - SynScan tests use `catch2_compat.h` for Catch2 include compatibility.

## [0.11.0] - 2026-02-19

### Added
- **vcpkg support** (Workspace)
  - Optional vcpkg integration for dependencies (e.g. curl for WeeWX). Controlled by `ALPACABRIDGE_ENABLE_VCPKG` (default ON on Windows). Scripts bootstrap vcpkg under user home if missing.
  - Root `vcpkg.json` manifest (e.g. curl dependency) for manifest mode.
- **Editor and repo hygiene**
  - `.editorconfig` for charset, line endings (LF; CRLF for `.bat`/`.cmd`/`.ps1`), final newline, trim trailing whitespace.
  - `.gitattributes` for normalized line endings (`* text=auto eol=lf`, CRLF for Windows scripts, LF for shell/CMake).

### Changed
- **SynScan Telescope Driver** (AlpacaCore)
  - Implemented `get_axis_rate_ranges` (vector of rate ranges per ASCOM). Tertiary axis returns empty set; primary/secondary return single range. Axis validation in `get_axis_rate_range` for invalid axis.
  - Added unit tests for axis rate range behavior.
- **Build and test scripts** (AlpacaBridge)
  - `build_and_run.cmd`: vcpkg toolchain and manifest dir when vcpkg enabled; robust `ROOT_DIR` resolution; delayed expansion for vcpkg paths.
  - `run_all_tests.cmd` / `run_all_tests.sh`: aligned with vcpkg-aware build when enabled.
- **.gitignore**
  - Ignore `build-curl-check/` (build/check artifact).

## [0.10.0] - 2026-02-03

### Added
- **WeeWX ObservingConditions Driver** (AlpacaCore)
  - Added WeeWX-based ObservingConditions driver and vendor wrapper (HTTP JSON feed via libcurl).
  - Added unit tests for WeeWX observing conditions.
  - Added WeeWX ConformU validation logs under `conformu/ObservingConditions/WeeWX/`.
- **WeeWX Device Support** (AlpacaHTTP)
  - Added routing and configuration support for WeeWX observing conditions devices.
  - Added web UI configuration fields for WeeWX devices.

### Changed
- **Supported Drivers Documentation** (AlpacaCore)
  - ObservingConditions section updated with WeeWX table, ConformU link, and driver notes.
- **Build & Router** (AlpacaCore, AlpacaHTTP)
  - CMake option `ALPACACORE_ENABLE_WEEWX` and router/config updates for ObservingConditions.

## [0.9.0] - 2026-02-03

### Added
- **SynScan Telescope Driver** (AlpacaCore)
  - Added Sky-Watcher SynScan telescope driver implementation and tests.
  - Added SynScan ConformU validation logs.
- **Protocol Reference Docs** (AlpacaCore)
  - Added SynScan and mount command set reference notes under `external/`.
- **SynScan Device Support** (AlpacaHTTP)
  - Added routing and configuration support for SynScan mounts.
  - Added web UI configuration fields for SynScan devices.

### Changed
- **Supported Drivers Documentation** (AlpacaCore)
  - Documented SynScan V3/V4 mount support, tested connection path (USB/Serial hand controller, Orion Atlas EQ-G), and validated platform (macOS arm64 only).
- **Project Documentation** (Workspace)
  - Updated README with SynScan driver availability and setup notes.
- **Agent Instructions** (Workspace)
  - Updated AGENTS.md for SynScan and release workflow.

### Removed
- **Build Artifacts** (AlpacaCore)
  - Removed stray `build-synscan` build outputs from the repo root.

## [0.8.6] - 2026-01-20

### Changed
- **HTTP Request Handling** (AlpacaHTTP)
  - Merge multiple `Accept` headers into a single comma-delimited value.
- **ImageBytes Mapping** (AlpacaHTTP)
  - Standardized image-bytes element type codes for 64-bit variants and accepted `long`/`ulong` aliases.
  - Added visibility logging when `imagearray`/`imagearrayvariant` evaluate image-bytes negotiation.

## [0.8.5] - 2026-01-19

### Changed
- **iOptron Network Reliability** (AlpacaCore)
  - Added socket send/receive timeouts for TCP connections.
  - Allow partial responses when a terminator is not required to avoid unnecessary timeouts.
  - Treat missing :MS1/:MS2 replies over network as accepted slews with a warning, matching WiFi bridge behavior.

## [0.8.4] - 2026-01-17

### Added
- **ImageBytes Streaming** (AlpacaHTTP)
  - Added `application/imagebytes` handling so `camera.imagearray` can stream compact binary payloads instead of JSON when clients request it.
  - Honored `camera.imagearrayvariant` metadata, inferred transmission element widths, and included numeric metadata plus transaction IDs alongside the pixel data.
  - Streamed structured error payloads with the same metadata layout so Alpaca exceptions can still be parsed when image bytes responses fail.

## [0.8.3] - 2026-01-13

### Changed
- **Web UI Enhancements** (AlpacaHTTP)
  - Added collapsible device cards with expand/collapse toggle buttons
  - Improved device list organization with consistent device type ordering (telescope, camera, filterwheel, focuser, rotator, dome, switch)
  - Enhanced device sorting by type, device number, and name
  - Improved filter wheel configuration UI with slot count selector (5, 7, 8 slots, or custom)
  - Added filter wheel preset options with common filter names (Luminance, RGB, Ha, OIII, SII, Sloan filters, Clear, Dark, UV, IR)
  - Filter wheel preset lookup with alias support for flexible filter name matching
  - Filter wheel slot management with individual slot configuration rows
  - Advanced filter name editing moved to collapsible details section
  - Better visual organization of device information and settings
  - Enhanced CSS styling for device cards, toggles, and filter wheel controls

## [0.8.2] - 2026-01-12

### Added
- **Windows 11 x64 Platform Support** (AlpacaCore)
  - Added Windows 11 x64 verification and testing documentation
  - Windows support verified for all ZWO drivers (ASI cameras, EFW filter wheel, EAF focuser, CAA rotator)
  - Windows support verified for iOptron telescope mounts
  - Added Windows library binaries for ZWO CAA rotator SDK
  - Windows driver requirement documentation for ZWO ASI cameras
  - Updated SUPPORTED-DRIVERS.md with Windows 11 x64 platform support across all driver tables
  - Added Windows Notes section to General Notes with USB and serial connection guidance
- **Socket Utilities** (AlpacaHTTP)
  - New `socket_utils.h` header for cross-platform socket operations

### Changed
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with Windows 11 x64 verification status for all drivers
  - Added Windows driver requirement note for ZWO ASI cameras (driver must be installed from ZWO)
  - Updated Verified OS/Arch entries to include Windows 11 (x64) for all tested drivers
  - Removed EAF Pro from focuser table (not available for testing)
  - Enhanced Windows Notes section with USB and serial connection information
- **Build System** (AlpacaCore)
  - Updated ZWO CMakeLists.txt to support Windows library binaries
- **Discovery Protocol** (AlpacaHTTP)
  - Enhanced discovery protocol implementation
- **Server Implementation** (AlpacaHTTP)
  - Improved server socket handling and utilities
- **iOptron Protocol** (AlpacaCore)
  - Enhanced iOptron protocol wrapper and telescope driver implementation
- **Logging** (AlpacaCore)
  - Improved logging implementation
- **Build Scripts** (AlpacaBridge)
  - Updated Windows build script (`build_and_run.cmd`)

## [0.8.1] - 2026-01-11

### Added
- **Linux Installation Script** (AlpacaBridge)
  - New `install_alpaca_service.sh` script for Linux (arm64) systems
  - Supports `install`, `update`, `uninstall`, and `status` commands
  - Automatically builds AlpacaCore and AlpacaHTTP, installs udev rules, and creates systemd service
  - Configurable via environment variables (ALPACAHTTP_USE_BOOST_BEAST, ALPACACORE_ENABLE_ALL_VENDORS, ALPACA_INSTALL_UDEV_RULES, ALPACA_GIT_PULL, ALPACA_CLEAN_BUILD)
  - Auto-detects CPU cores for parallel builds
- **Linux ARMv8 Platform Support** (AlpacaCore)
  - Added ARMv8 (arm64) Linux library binaries for all ZWO drivers (CAA, EAF, EFW)
  - Marked Linux ARMv8 as tested and verified for all ZWO and iOptron drivers
  - Updated SUPPORTED-DRIVERS.md with Linux ARMv8 verification status
  - Added udev rules for ZWO CAA rotator devices
- **Parallel Test Execution** (AlpacaBridge)
  - Enabled parallel test execution in `run_all_tests.sh`
  - Auto-detects CPU cores (sysctl on macOS, nproc on Linux)
  - Uses parallel cmake builds and ctest execution for faster test runs

### Changed
- **Build System** (AlpacaCore)
  - Added libudev dependency detection and linking for ZWO driver on Linux
  - Prefer pkg-config for libudev detection, fallback to find_library
  - Require libudev on Linux (non-Apple) platforms
- **Build Scripts** (AlpacaBridge)
  - Added automatic udev rules installation in `build_and_run.sh` for Linux
  - Configurable via `ALPACA_INSTALL_UDEV_RULES` environment variable (default: ON)
  - Automatically finds and installs all `.rules` files from `external/` directory
  - Reloads udev rules and triggers after installation
- **Test Infrastructure** (AlpacaCore)
  - Updated all test files to use `catch2_compat.h` instead of `catch2/catch_all.hpp`
  - Improved test compatibility and consistency across test suite
- **Documentation** (AlpacaBridge)
  - Added installation section to README.md with install script documentation
  - Updated AGENTS.md with note about udev rules installation requirement
  - Updated SUPPORTED-DRIVERS.md with Linux ARMv8 testing notes and verification status
- **Git Configuration** (AlpacaCore)
  - Added vendor SDK allowlist rules in `.gitignore` for ZWO SDK directories (CAA, EFW, EAF)
  - Allows vendor SDK files to be tracked in repository for easier distribution

## [0.8.0] - 2026-01-10

### Added
- **ZWO EFW Filter Wheel Driver** (AlpacaCore)
  - Complete ZWO EFW (Electronic Filter Wheel) driver implementation with full ASCOM Alpaca FilterWheel API support
  - SDK wrapper layer for ZWO EFW SDK Version 1.8.4
  - Support for USB connection via libusb-1.0
  - Comprehensive filter wheel property support (position, names, focus offsets, slot count)
  - Position control with slot count validation
  - Filter name and focus offset management
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Filter wheel binding via `filterwheelId` or `filterwheelIndex` configuration
  - ConformU validated for EFW on macOS (arm64) with 0 errors and 0 issues
- **FilterWheel Device Support** (AlpacaHTTP)
  - Complete FilterWheel device method routing and dispatch
  - Support for all ASCOM Alpaca FilterWheel API methods (position, names, focusoffsets)
  - ZWO EFW filter wheel device registration and configuration
  - Filter wheel device discovery and management via web UI
  - Smart auto-numbering for filter wheel device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO EFW filter wheel device registration with validation
  - Filter wheel device registration with filter wheel binding support
  - Improved device configuration validation for filter wheel devices
  - Smart filter wheel index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Filter wheel device type support in device configuration
  - ZWO filter wheel index and ID configuration fields
  - Filter names textarea input for custom filter naming
  - Auto-fill support for filter wheel indices
  - Enhanced vendor-specific configuration UI for ZWO filter wheels
- **Build System** (AlpacaBridge)
  - Updated `.gitignore` to allow `AlpacaCore/external/ZWO` folder and subfolders
  - ZWO SDK files (CAA, EAF, and EFW) now included in repository for easier distribution

## [0.7.0] - 2026-01-09

### Added
- **ZWO CAA Rotator Driver** (AlpacaCore)
  - Complete ZWO CAA (Camera Angle Adjuster) rotator driver implementation with full ASCOM Alpaca Rotator API support
  - SDK wrapper layer for ZWO CAA SDK Version 1.5.9
  - Support for USB connection via libusb-1.0
  - Comprehensive rotator property support (position, mechanical position, target position, step size, reverse, etc.)
  - Absolute position control with mechanical position support
  - Rotator movement control (move absolute, move, move mechanical, halt)
  - Position synchronization with sync offset support
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Rotator binding via `rotatorId` or `rotatorIndex` configuration
  - ConformU validated for CAA on macOS (arm64) with 0 errors and 0 issues
- **Rotator Device Support** (AlpacaHTTP)
  - Complete Rotator device method routing and dispatch
  - Support for all ASCOM Alpaca Rotator API methods
  - ZWO CAA rotator device registration and configuration
  - Rotator device discovery and management via web UI
  - Smart auto-numbering for rotator device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO CAA rotator device registration with validation
  - Rotator device registration with rotator binding support
  - Improved device configuration validation for rotator devices
  - Smart rotator index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Rotator device type support in device configuration
  - ZWO rotator index and ID configuration fields
  - Auto-fill support for rotator indices
  - Enhanced vendor-specific configuration UI for ZWO rotators
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ZWO CAA rotator information
  - Reordered all driver sections to match ASCOM API device type order (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope)
  - Added placeholders for all ASCOM device types not yet implemented
  - Documented CAA SDK version and platform support
  - Added Linux USB permissions documentation for CAA devices

## [0.6.1] - 2026-01-08

### Added
- **ZWO Camera Support** (AlpacaCore)
  - Added ASI120MM Mini camera to supported devices list
  - ConformU validated for ASI120MM Mini on macOS (arm64)

### Changed
- **ZWO Camera Driver** (AlpacaCore)
  - Improved camera info preloading for faster device name resolution
  - Added camera info refresh mechanism to ensure accurate device information
  - Enhanced camera enumeration and info caching for better performance
- **iOptron Telescope Driver** (AlpacaCore)
  - Updated driver description to accurately reflect supported mount series
  - Improved description clarity for all supported iOptron mount models
- **Web UI** (AlpacaHTTP)
  - Added cache busting for device list loading to prevent stale data
  - Improved device list refresh reliability with timestamp-based cache control
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ASI120MM Mini camera
  - Enhanced building guide with workspace-level script documentation
  - Improved documentation for ZWO driver support (camera, switch, focuser)

## [0.6.0] - 2026-01-06

### Added
- **ZWO EAF Focuser Driver** (AlpacaCore)
  - Complete ZWO EAF (Electronic Auto Focuser) driver implementation with full ASCOM Alpaca Focuser API support
  - SDK wrapper layer for ZWO EAF Focuser SDK Version 1.7.7
  - Support for USB connection via libusb-1.0
  - Comprehensive focuser property support (position, max step, temperature, etc.)
  - Absolute position control with step range support
  - Focuser movement control (move, stop, is moving detection)
  - Device state telemetry and connection management
  - Asynchronous connection/disconnection support
  - Focuser binding via `focuserId` or `focuserIndex` configuration
  - Support for EAF and EAF Pro models
  - ConformU validated for EAF and EAF Pro on macOS (arm64)
  - Note: EAF Pro Bluetooth version currently only works with USB connection (Bluetooth support not yet implemented)
- **Focuser Device Support** (AlpacaHTTP)
  - Complete Focuser device method routing and dispatch
  - Support for all ASCOM Alpaca Focuser API methods
  - ZWO EAF focuser device registration and configuration
  - Focuser device discovery and management via web UI
  - Smart auto-numbering for focuser device numbers and indices

### Changed
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO EAF focuser device registration with validation
  - Focuser device registration with focuser binding support
  - Improved device configuration validation for focuser devices
  - Smart focuser index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Focuser device type support in device configuration
  - ZWO focuser index and ID configuration fields
  - Auto-fill support for focuser indices
  - Enhanced vendor-specific configuration UI for ZWO focusers
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with ZWO EAF focuser information
  - Added Focuser Drivers section to supported drivers documentation
  - Documented EAF SDK version and platform support
  - Added note about EAF Pro Bluetooth limitation (USB only)

## [0.5.0] - 2026-01-05

### Added
- **ZWO Camera Driver** (AlpacaCore)
  - Complete ZWO ASI camera driver implementation with full ASCOM Alpaca Camera API support
  - SDK wrapper layer for ZWO ASI Camera SDK Version 1.40
  - Support for USB connection via libusb-1.0
  - Comprehensive camera property support (binning, ROI, gain, offset, temperature, etc.)
  - Exposure control with light/dark frame support
  - Pulse guiding support for autoguiding
  - Image array retrieval with optimized payload building
  - Asynchronous pulse guide implementation
  - Device state telemetry and connection management
  - ConformU validated for 6 camera models:
    - ASI174MM Mini
    - ASI290MM Mini
    - ASI462MM
    - ASI662MC
    - ASI2600MC Pro
    - ASI2600MM Pro
- **ZWO Switch Driver** (AlpacaCore)
  - Dew heater switch device implementation for ZWO cameras with anti-dew heater support
  - Automatic detection of cameras with `ASI_ANTI_DEW_HEATER` SDK control
  - Camera binding via `cameraId` or `cameraIndex` configuration
  - Full ASCOM Alpaca Switch API implementation (ISwitchV3)
  - Asynchronous switch state change support
  - ConformU validated for dew heater on:
    - ASI2600MC Pro
    - ASI2600MM Pro
- **Switch Device Support** (AlpacaHTTP)
  - Complete Switch device method routing and dispatch
  - Support for all ASCOM Alpaca Switch API methods
  - ZWO dew heater switch device registration and configuration
  - Switch device discovery and management via web UI
- **Server Restart Functionality** (AlpacaHTTP)
  - `/management/v1/restart` endpoint for graceful server restart
  - Restart callback support for custom restart handling
  - Thread-safe restart request handling with duplicate request prevention
  - Automatic server restart with connection cleanup and reinitialization
  - Restart button in web UI for easy server management
- **Smart Auto-Numbering** (AlpacaHTTP)
  - Automatic device number assignment based on existing devices
  - Smart camera index auto-fill for ZWO cameras
  - Automatic detection of next available device number per device type
  - User-modified field detection to preserve manual entries
  - Real-time auto-fill as device type changes in web UI
- **Transaction ID Management** (AlpacaHTTP)
  - Automatic server transaction ID generation using atomic counter
  - Thread-safe transaction ID assignment for concurrent requests
  - Client transaction ID parsing from query parameters, JSON bodies, and form data
  - Case-insensitive transaction ID extraction from form data
  - Proper transaction ID propagation in all Alpaca responses
- **Image Array Optimization** (AlpacaHTTP)
  - Optimized image array JSON payload building with pre-allocated buffers
  - Efficient integer-to-string conversion for large image arrays
  - Support for 2D and 3D image arrays (monochrome and color)
  - Improved performance for image transfer over HTTP
  - Direct string building to avoid JSON library overhead for large arrays
- **ConformU Validation Infrastructure** (AlpacaCore)
  - Comprehensive ConformU test results for all verified drivers
  - Test result organization by vendor and device type
  - Documentation of validated devices in SUPPORTED-DRIVERS.md
  - ConformU validation date tracking for all certified devices
- **Workspace Infrastructure** (AlpacaBridge)
  - Added AGENTS.md file with instructions for AI agents and Cursor workflows
  - Centralized reference to Cursor rules files for AlpacaCore and AlpacaHTTP
  - Clear documentation of agent workflow requirements and rule locations

### Changed
- **Router Architecture** (AlpacaHTTP)
  - Enhanced error handling to maintain HTTP 200 status for Alpaca error responses
  - Added `cancelasync` method to Switch device method set
  - Improved switch device method routing and parameter handling
  - Better device registration error messages for ZWO devices
  - Improved transaction ID extraction from multiple request sources (query, JSON, form)
  - Enhanced parameter parsing for query parameters, JSON bodies, and form data
- **Device Registration** (AlpacaHTTP)
  - Enhanced ZWO camera device registration with validation
  - ZWO switch device registration with camera binding support
  - Improved device configuration validation and error reporting
  - Smart device number and camera index auto-fill in web UI
- **Web UI Enhancements** (AlpacaHTTP)
  - Smart auto-numbering for device numbers and camera indices
  - Automatic next available number detection per device type
  - ZWO switch type selection (dew heater) in device configuration
  - Server restart button in server management interface
  - Improved device configuration form with auto-fill capabilities
  - Better handling of device editing vs. new device creation
  - Enhanced vendor-specific configuration UI for ZWO devices
- **Documentation** (AlpacaCore)
  - Updated SUPPORTED-DRIVERS.md with all ConformU-validated ZWO cameras and switches
  - Added Switch Drivers section to supported drivers documentation
  - Updated ConformU README with ZWO test results
  - Documented dew heater switch functionality and camera binding

## [0.4.0] - 2026-01-03

### Added
- **Logging System Enhancements** (AlpacaCore & AlpacaHTTP)
  - Log level filtering with configurable minimum log level
  - Log history capture with configurable history limit
  - External log sink support for custom logging integrations
  - `/management/v1/logs` endpoint for retrieving log history
  - `/management/v1/loglevel` endpoint for dynamic log level control
  - Log level applied at initialization from configuration
- **Web UI Enhancements** (AlpacaHTTP)
  - Interactive log level toggles with real-time updates
  - Log history display in web interface
  - Logo and improved styling throughout the UI
  - Device settings interface with configuration controls
  - Enhanced UI cleanup and user experience improvements
  - Improved server info display with formatted grid layout
  - Editable server location field with inline save functionality
  - Better JSON response parsing and error handling in UI
- **Device Persistence** (AlpacaHTTP)
  - Automatic device configuration persistence to `config/registered_devices.json`
  - Device registration persists across server restarts
  - Device removal and configuration management via API
- **Thread Pool Support** (AlpacaHTTP)
  - Configurable thread pool for concurrent request handling
  - Default pool size of 32 threads (configurable via environment variable)
  - Improved performance for multiple concurrent clients
- **Error Code Support** (AlpacaCore)
  - Error code support in `AlpacaException` for proper ASCOM error mapping
  - Improved error code mapping from AlpacaCore to HTTP responses
  - Better error reporting and debugging capabilities
- **Discovery Protocol Improvements** (AlpacaHTTP)
  - JSON response format for Alpaca Discovery protocol
  - Enhanced discovery compatibility with ASCOM clients
- **Server Location Management** (AlpacaHTTP)
  - PUT/POST support for `/management/v1/description` endpoint to update server location
  - Automatic persistence of location changes to YAML configuration file
  - Thread-safe server information management with configurable server name, manufacturer, and version
- **Version Information** (AlpacaHTTP)
  - Version header file (`version.h`) for compile-time version access
  - Centralized version management in CMake build system
- **Workspace Infrastructure** (AlpacaBridge)
  - Consolidated CHANGELOG.md at workspace level
  - Comprehensive README.md with logo, quick start guide, and build instructions
  - Cross-platform build and test scripts (`build_and_run.sh/cmd`, `run_all_tests.sh/cmd`)
  - Workspace-level `.gitignore` for build artifacts and local configuration
  - Workspace-level LICENSE file

### Changed
- **Router Architecture** (AlpacaHTTP)
  - Enhanced parameter parsing for query parameters and JSON request bodies
  - Auto-parse JSON strings in response values for better compatibility
  - Improved form parsing for device configuration
  - Better error responses with proper Alpaca error codes
  - Case-insensitive query parameter lookup support
  - YAML configuration file editing for server location persistence
  - Improved error status application with proper HTTP status code mapping
- **iOptron Telescope Driver** (AlpacaCore)
  - Improved status parsing and command handling
  - Enhanced slew completion detection
  - Better protocol compliance and error handling
  - ConformU validation and certification updates
- **Build System** (AlpacaCore & AlpacaHTTP)
  - Added `clean-all` target for complete build cleanup
  - Vendor compile definitions for better SDK integration
  - Fixed circular dependency warnings in CMake
- **Documentation** (AlpacaCore & AlpacaHTTP)
  - Updated cursor rules for development workflow
  - Enhanced external documentation
  - Added full SSPL v1 license text to repository

## [0.3.0] - 2025-12-16

### Added
- **iOptron Telescope Driver** (AlpacaCore 0.3.0)
  - Complete telescope driver implementation for iOptron mounts
  - Protocol wrapper for RS-232 command set over serial or TCP
  - Support for position, motion, tracking, and site information
- **Web UI** (AlpacaHTTP 0.3.0)
  - Modern web-based device management interface
  - Device listing, configuration, and status display
  - Accessible at root path (`/`) and `/web/` routes
- **Telescope Driver Support** (AlpacaHTTP 0.3.0)
  - Comprehensive telescope method dispatch (50+ methods)
  - Full ASCOM Alpaca Telescope API implementation
- **Management API Enhancements** (AlpacaHTTP 0.3.0)
  - API version discovery endpoint
  - Device configuration and removal endpoints
  - Graceful server shutdown support
- **Comprehensive Developer Documentation** (AlpacaCore 0.3.0)
  - Complete building guide with prerequisites and troubleshooting
  - Driver development guide with three-layer architecture
  - Testing guide with comprehensive coverage
  - Architecture documentation

### Changed
- **Build System** (AlpacaCore 0.3.0)
  - Added `ALPACACORE_ENABLE_ALL_VENDORS` option
  - Enhanced vendor library detection and installation
  - Improved CMake namespace aliasing
- **Router Architecture** (AlpacaHTTP 0.3.0)
  - Major expansion with telescope-specific method routing
  - Enhanced parameter parsing for query params and JSON bodies
  - Improved error responses with proper Alpaca error codes
- **Testing Infrastructure** (AlpacaCore 0.3.0)
  - Updated to modern Catch2 integration
  - Enhanced test file consistency

## [0.2.1] - 2025-12-04

### Changed
- **License Headers** (AlpacaHTTP 0.2.1)
  - Updated all source files with new license header format
  - Changed license URL to GitHub repository location
  - Added SSPL v1 compliance notice to all headers

## [0.2.0] - 2025-12-02

### Added
- **DeviceRegistry** (AlpacaCore 0.2.0)
  - Complete device management system with singleton pattern
  - Thread-safe device registration and lookup
  - Device capability enumeration for management API
- **AlpacaCore Integration** (AlpacaHTTP 0.2.0)
  - Full integration with AlpacaCore device registry
  - Device method dispatch for common AlpacaDriver methods
  - Management Driver support for server information

### Changed
- **License URLs** (AlpacaCore 0.2.0)
  - Updated license URL in all source files to GitHub repository location
  - Updated 49 files (headers, sources, and tests)
- **CMake Integration** (AlpacaHTTP 0.2.0)
  - Updated to link against AlpacaCore
  - Supports both installed AlpacaCore and workspace builds
- **Router Architecture** (AlpacaHTTP 0.2.0)
  - Refactored to use AlpacaCore interfaces
  - Replaced placeholder DeviceManager with AlpacaCore DeviceRegistry

## [0.1.0] - 2025-12-02

### Added
- **Initial Release**
  - AlpacaCore: Core Alpaca protocol library with device driver interfaces
  - AlpacaHTTP: HTTP/1.1 server with Alpaca API routing
  - Complete directory structure following architecture guidelines
  - CMake build system with C++20 support
  - Test infrastructure with Catch2 support
  - Example servers and device implementations
  - Comprehensive documentation

