# AlpacaBridge Supported Drivers

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

## Updated 2026-05-29
This document lists all hardware vendors and device types that are verified to work with AlpacaBridge.

## General Notes

- **ConformU Verification**: All drivers listed below have been tested and verified using the ConformU tool to ensure full compliance with the ASCOM Alpaca API specification.
- **Driver Status**: Only drivers that have been verified with ConformU are listed. Additional drivers may be in development but are not included until they pass ConformU verification.
- **Adding New Drivers**: New driver support can be added by implementing the appropriate driver interface. See the [Development Guide](docs/development.md) for details. All drivers must pass ConformU verification before being added to this list.

- **Connection Types**:
  - **Ethernet**: Network-based connection (TCP/IP)
  - **USB/Serial**: USB-to-serial adapter or direct serial connection

- **Linux Notes**:
  - **Debian 13 (Trixie) on arm64**: AlpacaBridge is built and validated on arm64 only (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate). All drivers have been tested using Debian 13 on arm64 with ConformU v4.2.1 (original drivers) or v4.3.0 (newer drivers). As new ConformU versions are released this will be adjusted.
  - **Kernel 6.12.75-v8-16+ or higher.**: Note: kernel 6.12.75-v8-16+ is required to ensure ZWO EAF/EFW hardware compatibility. Without it, devices besides ZWO may or may not be recognized. Please check the kernel version.

- **Wi-Fi / Mount Notes**:
  - **Debian 13 (Trixie)**: Wi-Fi has been tested from Raspberry Pi to the mount. Due to the limited Wi-Fi power management on the Raspberry Pi, it is highly recommended to disable low power mode if you opt to connect via Wi-Fi to the mount. A USB connection to the mount is recommended when possible, as commands are much quicker and more reliable.

## Camera Drivers

### Player One

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Ceres 462M | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Player%20One/Ceres%20462M/) |

### Player One Driver Notes

- **SDK**: Player One Camera SDK v3.10.0 (build target)
- **Connection**: USB (requires udev rules `99-player_one_astronomy.rules`)
- **Tested model**: Ceres 462M (uncooled) on Linux arm64.
- **Cooling (TEC)**: Capability-gated on the SDK's `POA_COOLER` / `POA_TARGET_TEMP` config attributes. Uncooled cameras report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`. Cooler control paths (`CoolerOn`, `SetCCDTemperature`, `CoolerPower`) are implemented but untested against physical cooled hardware.
- **Dew Heater**: Not exposed. The SDK advertises `POA_HEATER_POWER` on cooled models, but we do not currently have a cooled Player One camera available to implement and validate the Switch device. Will be added when hardware is available.
- **Pulse guiding**: Capability-gated on `isHasST4Port`. Driver times the pulse duration via `POA_GUIDE_NORTH/SOUTH/EAST/WEST` bool toggles.

### QHY

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| QHY268C | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/QHY/QHY268C/) |

### QHY Driver Notes

- **SDK**: QHY CCD SDK 25.09.29 (build target)
- **Connection**: USB (requires udev rules and firmware; see below)
- **Cooler power**: `CanGetCoolerPower` returns false; cooler power reporting is not implemented to avoid SDK timeouts.

### SVBONY

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| SV905C2 | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/SVBONY/SV905C2/) |

### SVBONY Driver Notes

- **SDK**: SVBONY Camera SDK v1.13.4 (build target) 
- **Connection**: USB (requires udev rules `90-ckusb.rules`)

### ToupTek

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| GPCMOS01200KPF | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/GPCMOS01200KPF/) |

### ToupTek Driver Notes

- **SDK**: ToupTek toupcamsdk 2026-01-28 (build target)
- **Connection**: USB (self-contained `libtoupcam.so`; no libusb/libudev link dependency)
- **Tested model**: GPCMOS01200KPF (guide camera) on Linux arm64. Other ToupTek models sharing the same SDK are expected to work but have not been individually ConformU-verified.
- **Dew Heater**: Not supported. The ToupTek SDK exposes `TOUPCAM_FLAG_HEAT` / `TOUPCAM_OPTION_HEAT` for anti-fog heating on cooled cameras, but we do not currently have a cooled ToupTek camera available to implement and validate the Switch device. Will be added when hardware is available.
- **Cooling (TEC)**: Capability-gated on the SDK's `TOUPCAM_FLAG_TEC` / `TOUPCAM_FLAG_TEC_ONOFF` flags. Uncooled cameras report `CanSetCCDTemperature = false` and `CanGetCoolerPower = false`. Cooler control paths are implemented but untested against physical cooled hardware.

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| ASI120MM Mini | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI120MM%20Mini/) |
| ASI174MM Mini | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI174MM%20Mini/) |
| ASI2600MC Pro | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MC%20Pro/) |
| ASI2600MM Pro | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI2600MM%20Pro/) |
| ASI290MM Mini | USB |  | pending arm64 re-validation |
| ASI462MM | USB |  | pending arm64 re-validation |
| ASI662MC | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASI/ASI662MC/) |

### ZWO Driver Notes

- **SDK**: ZWO ASI Camera SDK Version 1.40 (build target)
- **Connection**: USB (requires libusb-1.0)
- **Dew Heater**: Exposed as a Switch device (`switchType: dewheater`) when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Use `cameraId` or `cameraIndex` to bind to the target camera.

## CoverCalibrator Drivers

*No drivers currently available.*

## Dome Drivers

*No drivers currently available.*

## FilterWheel Drivers

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| EFW | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EFW/) |

### ZWO FilterWheel Driver Notes

- **SDK**: ZWO EFW SDK Version 1.8.4 (build target)
- **Connection**: USB (requires libusb-1.0)

## Focuser Drivers

### Gemini

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Gemini Automatic Astro Focuser Pro | USB/Serial | ✓ | [ConformU Validation](AlpacaCore/conformu/Gemini/Astro%20Focuser%20Pro/) |

### Gemini Focuser Driver Notes

- **Protocol**: MyFocuserPro2 serial protocol (no SDK required)
- **Connection**: USB/Serial (CH340/CH341 adapter). Auto-detection supported.
- **Auto-detection**: Scans `/dev/serial/by-id/` for CH340/CH341 USB-serial devices and probes with firmware handshake. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.

### ToupTek

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| AAF (Astro Auto Focuser) | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ToupTek/AAF/) |

### ToupTek Focuser Driver Notes

- **SDK**: ToupTek toupcamsdk 2026-01-28 (shared with the ToupTek camera driver)
- **Connection**: USB. Devices are enumerated via `Toupcam_EnumV2` and filtered by `TOUPCAM_FLAG_AUTOFOCUSER`.
- **Configuration**: `focuserIndex` (0-based among AAF devices) or `focuserId` (opaque SDK id; overrides index).
- **Capabilities**: Absolute positioning, halt, max-step query, on-board temperature (tenths of °C), backlash and reverse direction supported by the firmware. `StepSize` is not exposed because the AAF firmware does not report mechanically-valid microns-per-step for arbitrary focuser setups.
- **Temperature compensation**: Not implemented — the AAF action set does not expose a temp-comp control.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues on Linux arm64.

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| EAF | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/EAF/) |

### ZWO Focuser Driver Notes

- **SDK**: ZWO EAF Focuser SDK Version 1.7.7 (build target)
- **Connection**: USB (requires libusb-1.0)
- **EAF Pro Bluetooth**: The ZWO EAF Pro Bluetooth version will only currently work with USB connection. Bluetooth support is not yet implemented.

## ObservingConditions Drivers

### WeeWX

| Source | Connection | Linux<br>(arm64) | Status |
|--------|------------|------------------|--------|
| WeeWX HTTP JSON | HTTP(S) | ✓ | [ConformU Validation](AlpacaCore/conformu/WeeWX/) |

### WeeWX ObservingConditions Driver Notes

- **Source**: WeeWX HTTP JSON feed (`lcd_datasheet.current`); missing sensors return NaN.
- **Connection**: HTTP(S) to WeeWX REST/JSON endpoint.
- **Configuration**: `weewxUrl` (required), optional `pollIntervalSeconds`, `timeoutMs`.

## Rotator Drivers

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| CAA | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/CAA/) |

### ZWO Rotator Driver Notes

- **SDK**: ZWO CAA SDK Version 1.5.9 (build target)
- **Connection**: USB (requires libusb-1.0)

## SafetyMonitor Drivers

*No drivers currently available.*

## Switch Drivers

### ZWO

| Device Type | Model Series | Connection | Linux<br>(arm64) | Status |
|-------------|--------------|------------|------------------|--------|
| Dew Heater | ASI2600MC Pro | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |
| Dew Heater | ASI2600MM Pro | USB (via Camera) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/Dew%20Heater%20Switch/) |
| ASIair Pro 12V Power | ASIair Pro (Pi 4) | Local GPIO (libgpiod v2) | ✓ | [ConformU Validation](AlpacaCore/conformu/ZWO/ASIair%20Pro/) |
| ASIair Plus 12V Power | ASIair Plus (RK3568) | ZWO `pwm_gpio.ko` ioctl | Pending | ConformU pending |

### ZWO Switch Driver Notes

- **Device Type: Dew Heater** (`switchType: dewheater`) — exposed when the camera reports the SDK control `ASI_ANTI_DEW_HEATER`. Bind to a camera via `cameraIndex` or `cameraId`.
- **Device Type: ASIair Pro 12V Power** (`switchType: asiair`) — controls the four on-board 12V DC outputs directly via Linux GPIO using libgpiod v2. Default Pi 4 ASIair Pro layout: Port 1 = GPIO 12, Port 2 = GPIO 13, Port 3 = GPIO 26, Port 4 = GPIO 18 on `/dev/gpiochip0`. All ports default to boolean on/off; mark a port `"pwm": true` in the config to expose it as a 0–100% software PWM channel (default 1 kHz, configurable via `pwmFrequencyHz`). Requires the AlpacaBridge daemon to run on the ASIair Pro itself with the stock pigpiod-based ZWO daemons disabled; the driver requires arm64 so the stock Raspbian Buster armv7l OS must be re-imaged with Raspberry Pi OS 64-bit (Bookworm or Trixie) before this driver can be deployed. **Step-by-step setup**: see [AlpacaCore/PowerPorts.md](AlpacaCore/PowerPorts.md).
- **ASIair Pro ConformU**: 4.3.0 — 0 errors, 0 issues, 0 timing issues on Linux arm64 (Debian 13 Trixie, kernel 6.18.29+rpt-rpi-v8). Tested with a mixed config (2 boolean ports + 2 PWM ports) so both code paths were exercised in a single run. Slowest member 21 ms (well under STANDARD 1 s target).
- **Device Type: ASIair Plus 12V Power** (`switchType: asiair-plus-rk3568`) — controls the four on-board 12V DC outputs of the Rockchip RK3568 ASIair Plus via ZWO's custom `pwm_gpio.ko` kernel module. The driver opens `/dev/pwm-gpio-misc` and uses the documented ioctl interface (header reverse-engineered and vendored at `AlpacaCore/external/ZWO/asiair-plus/pwm_gpio.h`). Wrapper indices 0–3 map to kernel ioctl indices 4–7 (DC1–DC4). PWM is **hardware-driven via kernel hrtimer** (period/duty in nanoseconds) — much lower CPU than the userspace soft-PWM the Pi 4 driver uses. PWM frequency range 1–100,000 Hz validated empirically against the hrtimer; default 1 kHz. The driver depends on the ZWO kernel module remaining loaded, so the device must keep the ZWO kernel (4.19.219) — mainline RK3568 distros do not ship `pwm_gpio.ko`. A new udev rule (`99-zwo-asiair-plus.rules`) grants the `gpio` group `0660` access to `/dev/pwm-gpio-misc`; install it and add the AlpacaBridge user to the `gpio` group. **Step-by-step setup**: see [AlpacaCore/PowerPorts.md](AlpacaCore/PowerPorts.md). **ConformU**: pending on-device validation.

## Telescope Drivers

### Celestron

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| CGX-L | USB/Serial (hand controller) | ✓ | [ConformU Validation](AlpacaCore/conformu/Celestron/) |

### Celestron Driver Notes

- **Protocol**: NexStar HC serial protocol with MC passthrough (P-command) for per-axis control.
- **Connection**: USB/Serial via hand controller.
- **Required firmware**: Driver is built and tested against **HC (GEM) 5.35.3179** and **MC 7.18.5020**. Other firmware versions are not supported — behavior on earlier or later firmware has not been validated and may differ in slew, tracking, and pier-side semantics.
- **Required HC startup**: Power mount on, press Enter through Switch Position → Location → select **Last Alignment** → Enter. HC must show **"CGX-L Ready"** before connecting the driver. Slews are refused until the mount reports aligned or a `SyncToCoordinates` has been performed in the current driver session.
- **Location/time**: `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped once the mount is aligned — applying them would invalidate the HC alignment model (especially StarSense). Writes succeed from the client's perspective but do not touch the mount. Set these before completing alignment if you need them to take effect.
- **RA slew offset**: Driver learns a running-average RA undershoot correction and pre-biases subsequent slews (adaptation matches INDI's `SlewOffsetRa`). CGX-L fw 7.18 does not track during a goto, causing consistent RA undershoot without this compensation.
- **Post-slew tracking**: Tracking is re-asserted via the top-level `T` set-tracking-mode command after each slew completes; CGX-L fw 7.18 does not auto-resume tracking after goto.
- **Pulse guiding**: Uses native MC_AUX_GUIDE (0x26) hardware command via the autoguider port. The firmware times the pulse internally — no software sleep or encoder math required. Position hold/correction pattern bridges the gap between the low-level firmware command and ASCOM coordinate expectations.
- **Pier side**: `SideOfPier` reports actual pier side via the HC `p` command (`W` = pierWest, `E` = pierEast).

### iOptron

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| HEM27 series | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/HEM27) |
| HAE43 series | USB/Serial, Wi-Fi | ✓ | [ConformU Validation](AlpacaCore/conformu/iOptron/HAE43) |


### iOptron Driver Notes

- **Protocol**: iOptron Mount RS-232 Command Language Version 3.10 (January 4th, 2021)
- **Connection**: USB/Serial or Wi-Fi (TCP). Auto-detection supported for both — `connectionType: "auto"` scans serial ports first, then falls back to network discovery if no serial mount is found.
- **Serial auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, Silicon Labs, and generic USB-serial devices and probes each with an iOptron `:MountInfo#` query at 115200 baud. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`. The 4-byte model code response (no `#` terminator) is mapped to a human-readable mount name (e.g., `0025` → HEM27) using the current INDI v3 model table.
- **Network auto-detection**: When the connection type is set to Network/Auto, the driver runs a multi-phase discovery: (1) probes well-known iOptron Wi-Fi module addresses (`10.10.100.254`, `10.10.100.1`, `192.168.100.1`) on ports 8899 and 4030; (2) if no mount found, queries the default gateway on each local interface (iOptron mounts act as the AP gateway); (3) if still not found, scans all hosts on each local subnet (up to /24) with parallel non-blocking TCP connect probes. Each candidate is verified with a `:MountInfo#` query before being accepted.
- **Mount identification**: On connect, the driver queries `:MountInfo#` and maps the model code to a name displayed in `Name` (e.g., "iOptron HEM27"), `UniqueID`, and server logs. 60+ models supported including CEM, GEM, HEM, HAE, HAZ, and SkyHunter series.
- **Wi-Fi reliability**: Network (TCP) connections drain stale acknowledgment bytes from blind commands to prevent buffer accumulation on the mount's Wi-Fi module. `IsPulseGuiding` uses lock-free atomics to meet the ConformU fast response target over high-latency links.
- **Tested firmware**: Driver tested on **HEM27** with main board firmware **V240121** and hand controller firmware **V241201**. Other firmware versions and models may work but have not been individually verified.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues on both USB and Wi-Fi, on Linux arm64.

### SynScan V3/V4

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| Sky-Watcher HEQ5 PRO | USB/Serial (hand controller) | ✓ | [ConformU Validation](AlpacaCore/conformu/SynScan/Sky-Watcher%20HEQ5%20PRO/) |

### SynScan Driver Notes

- **Protocol**: Sky-Watcher SynScan V3/V4 protocol
- **Connection**: USB/Serial via hand controller (tested). Auto-detection supported — `connectionType: "auto"` scans serial ports for SynScan hand controllers and connects to the first responding mount.
- **Auto-detection**: Scans `/dev/serial/by-id/` for Prolific, FTDI, CP210x, and generic USB-serial devices and probes each with a SynScan firmware version query. Falls back to `/dev/ttyUSB0`–`/dev/ttyUSB9`.
- **Sky-Watcher HEQ5 PRO Firmware**: Hand controller firmware 4.42.00, motor controller firmware 3.46
- **Pulse guiding**: Software-timed variable-rate slew (SynScan has no hardware pulse guide command). Driver issues a variable-rate axis slew at the configured guide rate, times the pulse duration in a background thread, then stops the axis and restores sidereal tracking. GEM pier-side DEC direction flip applied automatically. Position reporting uses accumulated `rate × duration` deltas in the target coordinate frame for ConformU tolerance compliance.
- **ConformU**: Validated with ConformU 4.3.0 — 0 errors, 0 issues (pulse guide tested across N/S/E/W at declinations -9, +9, -3, +3).

### ZWO

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| AM3 | USB/Serial, Wi-Fi | ✓ | [ConformU](AlpacaCore/conformu/ZWO/AM3/) |
| AM5 | USB/Serial, Wi-Fi |  | pending arm64 re-validation |
| AM5N | USB/Serial, Wi-Fi |  | pending arm64 re-validation |
| AM7 | USB/Serial, Wi-Fi |  | pending arm64 re-validation |

### ZWO Telescope (ASI Mount) Driver Notes

- **Protocol**: ZWO Mount Serial Communication Protocol (see `AlpacaCore/external/ZWO/AM/ZWO_Mount_Protocol.md`)
- **Connection**: Serial over USB or network (TCP). **Tested and working with USB and WiFi**. PulseGuide and slew behavior validated over both USB and WiFi; timing tuned for high-latency (WiFi) connections.
- **Tested firmware**: Driver tested on ZWO **firmware 1.8.8\***. Other firmware versions and models (e.g., AM3, AM5, AM7) may work but have not been verified.
