# AlpacaBridge Changelog

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

## [1.0.4] - UNRELEASED

### Added
- **ASCOM Platform 7 interface versions + compliant DeviceState** (AlpacaCore): every driver now advertises its Platform 7 interface version and returns a spec-compliant `DeviceState` (the `connect`/`connecting`/`devicestate`/`disconnect` endpoints were already wired in AlpacaHTTP). `InterfaceVersion` bumped to ICameraV4 (4), ITelescopeV4 (4), IFocuserV4 (4), IRotatorV4 (4), and IObservingConditionsV2 (2); FilterWheel and Switch already reported their Platform 7 versions (IFilterWheelV3 / ISwitchV3). The per-vendor `DeviceState` overrides — which emitted non-standard names (`Connected`, `CoolerOn`) and omitted the `TimeStamp` — are replaced by one spec-compliant implementation in each device base class (`CameraDriver`, `TelescopeDriver`, `FocuserDriver`, `RotatorDriver`, `ObservingConditionsDriver`, `FilterWheelDriver`). Each base builds the operational-property list by calling the device's own property getters inside a try/catch (a property whose getter throws — `AlpacaException` or any unwrapped vendor `std::exception` — is omitted rather than failing the whole call), so `DeviceState` always agrees with the matching GET endpoint, which is the consistency ConformU verifies; a shared inline `device_state_timestamp()` helper appends the ISO 8601 `TimeStamp`. The base `get_device_state()` and the timestamp helper are defined inline so the device-class vtables stay weak and the per-vendor static libraries link without a base-library ordering dependency. Telescope omits `UTCDate` (optional "if known") to avoid format drift versus the `/utcdate` endpoint. Camera DeviceState reports `CameraState`, `CCDTemperature`, `CoolerPower`, `HeatSinkTemperature`, `ImageReady`, `IsPulseGuiding`, `PercentCompleted`. Per-driver `InterfaceVersion` unit assertions updated, disconnected-state tests updated to the new contract, and DeviceState regression tests added (TimeStamp present, no `Connected`/`CoolerOn`, all names valid, and omit-on-throw covering unwrapped vendor exceptions). Full AlpacaCore suite green (179 tests, vendors ON). **Pending ConformU V4 hardware validation per device before release.**
- **ZWO ASI290MM Mini ConformU 4.3.0 validated** on Linux arm64 at **ICameraV4**: 0 errors, 0 issues, 0 timing issues, exercising the Platform 7 `DeviceState` (7 operational properties + `TimeStamp`) and the async `Connect`/`Disconnect` lifecycle through a full exposure/`ImageArray` cycle. Report saved to `AlpacaCore/conformu/ZWO/ASI/ASI290MM Mini/Linux-arm64.txt`; `SUPPORTED-DRIVERS.md` Camera row updated from "pending arm64 re-validation" to ✓.
- **JavaScript syntax gate** (CI + `scripts/ci_preflight.sh`): the hand-written web UI JS (`AlpacaHTTP/web/*.js`) is served static with no bundler/build step and previously had no automated validation. A new `javascript` CI job and a matching pre-flight gate run `node --check` on each web JS file (parse-only; no execution), catching syntax errors before they ship. CI checks all web JS on every run; the pre-flight checks it only when web JS changed (auto-installing `nodejs` like the other tools), mirroring the `shellcheck` pattern.
- **ZWO ASIair Pro Switch Driver** (AlpacaCore): new `SwitchDriver` implementation exposing the four on-board 12V DC power ports of the ZWO ASIair Pro (Pi 4 / BCM2711) as ASCOM Switch channels. Per-port mode is independently configurable as boolean on/off or 0–100% software PWM at a configurable frequency (default 1 kHz). Default Pi 4 ASIair Pro layout: Port 1=GPIO 12, Port 2=GPIO 13, Port 3=GPIO 26, Port 4=GPIO 18 on `/dev/gpiochip0`. Implemented as a 4th-file protocol wrapper (`zwo_asiair_protocol_wrapper`) over **libgpiod v2** that owns all four lines via a single `gpiod_line_request` and spawns per-port worker threads for soft-PWM channels.
- **ZWO ASIair Pro Device Support** (AlpacaHTTP): router accepts `vendor=zwo deviceType=switch switchType=asiair` with optional `gpioChip`, `pwmFrequencyHz`, and `ports[]` overrides for non-Pi-4 SBC deployments. Web UI Switch Type dropdown adds the new option, hides the camera-binding fields when ASIair is selected, and shows a per-port configuration table (channel name, GPIO line, PWM checkbox) plus the GPIO chip path and PWM frequency inputs. Marking a port PWM exposes it to clients (NINA etc.) as a 0–100% slider under "Gauges" instead of a plain on/off toggle, enabling dew heater and flat panel use. Defaults preserve the one-click flow for the standard Pi 4 ASIair Pro layout.
- **ZWO ASIair Pro Unit Tests**: 9 test cases, 74 assertions covering defaults, device metadata, per-port metadata (channel ranges, descriptions, default names), `NotConnected`/`InvalidValue` error codes, switch ID range validation, state machine when disconnected, unsupported method error codes, and constructor rejection of invalid configs (empty ports, zero PWM frequency, out-of-range frequency).
- **Packaging** (debian/control): `libgpiod-dev (>= 2.0)` added to Build-Depends; `libgpiod3` added to runtime Depends.
- **Power port setup guide** (AlpacaCore): new `AlpacaCore/PowerPorts.md` with copy-paste install + Web UI configuration instructions for the ZWO ASIair Pro 12V power switch. Stubbed sections for ASIair Plus (RPi CM4), ASIair Plus (Rockchip RK3568), ToupTek StellaVita (Pi CM4), and iOptron iMate to be filled in as those controllers validate. Cross-linked from `SUPPORTED-DRIVERS.md` and `AGENTS.md`.
- **ZWO ASIair Pro ConformU 4.3.0 validated** on Linux arm64 (Debian 13 Trixie, kernel 6.18.29+rpt-rpi-v8): 0 errors, 0 issues, 0 timing issues. Tested with a mixed config (2 boolean + 2 PWM ports) so both code paths were exercised in one run. Slowest member 21 ms vs the STANDARD 1 s target — wide timing margin throughout. Results saved to `AlpacaCore/conformu/ZWO/ASIair Pro/Linux-arm64.txt` + `.report.json`; `SUPPORTED-DRIVERS.md` Switch row updated from `Pending re-image` to ✓.
- **ZWO ASIair Plus (RK3568) Switch Driver** (AlpacaCore): new `SwitchDriver` implementation for the Rockchip RK3568 variant of the ASIair Plus. Talks to ZWO's custom `pwm_gpio.ko` kernel module via ioctls on `/dev/pwm-gpio-misc` (header reverse-engineered and vendored at `AlpacaCore/external/ZWO/asiair-plus/pwm_gpio.h`). Exposes the four 12V DC ports as ASCOM Switch channels with per-port boolean on/off or **userspace soft-PWM** via a `pwm_loop` worker thread per PWM-enabled port (default 50 Hz, matching what ZWO's stock `zwoair_imager` daemon actually drives the kernel module at — confirmed by `PWM_GPIO_GET_CONFIG` against a live stock-firmware ASIair Plus; range 1–100,000 Hz configurable via `pwmFrequencyHz`). The kernel module's own hrtimer PWM dispatch turned out unreachable from any documented ioctl sequence and GPIO bank 4 has no hardware PWM mux on the RK3568, so software PWM is the only viable path; this matches the approach the extracted stock `zwoair_imager` daemon takes. The `SET_LEVEL` ioctl semantics are inverted from typical gpiod conventions (`level=0` ⇒ pad floats high ⇒ panel ON); the wrapper translates internally so ASCOM `value=1` always means on. Wrapper indices 0–3 map to kernel ioctl indices 4–7 (DC1–DC4). Connect-time policy is **read-only** — `open()` opens the fd and writes nothing; the first kernel write is the user's first `SetSwitch` / `SetSwitchValue` call, which avoids the master-enable-related power glitches earlier revisions of this driver hit on every reconnect. Disconnect leaves each port in its last-set state, same policy as the Pi 4 ASIair Pro driver. Independent driver from the libgpiod-based `asiair` — the kernel-interface differences are fundamental.
- **ZWO ASIair Plus (RK3568) Device Support** (AlpacaHTTP): router accepts `switchType: "asiair-plus-rk3568"` with `devicePath`, `pwmFrequencyHz`, and `ports[]` (channel name + PWM flag, no GPIO field since the kernel module fixes the mapping). Web UI Switch Type dropdown gains a third option "ASIair Plus 12V Power Switch (RK3568)" with a simpler per-port table than the Pro UI.
- **ZWO ASIair Plus Unit Tests**: 9 test cases / 75 assertions covering defaults, device metadata, per-port metadata, `NotConnected`/`InvalidValue` error codes, switch ID range validation, state machine when disconnected, unsupported method error codes, and constructor rejection of invalid configs (empty ports, > 4 ports, zero PWM frequency, out-of-range frequency).
- **ZWO ASIair Plus udev rule** (`AlpacaCore/external/ZWO/asiair-plus/99-zwo-asiair-plus.rules`): `KERNEL=="pwm-gpio-misc", MODE="0660", GROUP="gpio"` so the AlpacaBridge daemon can open the kernel-module device without root. Picked up automatically by the existing `build_and_run.sh` rules-install loop.
- **PowerPorts.md ASIair Plus (RK3568) section**: promoted from stub to full setup guide — kernel-module dependency, gpio-group membership, install steps, Web UI walk-through, advanced config schema, USB-power / button extensibility notes.
- **ZWO ASIAIR Plus (Pi CM4) Switch Support** (AlpacaHTTP): the CM4-based ASIAIR Plus shares the Pi 4 ASIAIR Pro's on-board libgpiod wiring — GPIO 12/13/26/18 on `/dev/gpiochip0` (BCM2711) — verified against live CM4 hardware by correlating the stock app's port settings (Port 1 ON, dew heater 59%, flat panel 34%, Port 4 ON) against the BCM GPIO bank (`pigs gdc` read back 590/1000 on GPIO13 and 340/1000 on GPIO26, exactly matching). It therefore reuses the existing libgpiod `asiair` switch driver with no new device logic. New `switchType: "asiair-plus-picm4"` is routed to `create_zwo_asiair_switch` / `default_asiair_pro_config()` with matching config sanitization, and the Web UI Switch Type dropdown gains an "ASIAIR Plus 12V Power Switch (Pi CM4)" option that reuses the Pro's per-port GPIO table. `AlpacaCore/PowerPorts.md` gains a full CM4 setup section (re-image to arm64, libgpiod deps, gpio group, optional default-on boot directive, device add, `gpioget` verification).
- **ZWO ASIAIR Plus (Pi CM4) ConformU 4.3.0 validated** on Linux arm64 (Raspberry Pi Compute Module 4, BCM2711): 0 errors, 0 issues, 0 timing issues, run against the live re-imaged CM4 with all four 12V DC ports exercised. Slowest member 15 ms (15% of the FAST 0.1 s target). Results saved to `AlpacaCore/conformu/ZWO/ASIair Plus (Pi CM4)/Linux-arm64.txt`; `SUPPORTED-DRIVERS.md` Switch row added with ✓.
- **iOptron iMate PowerBox Switch Driver** (AlpacaCore): new `SwitchDriver` exposing the iMate's on-board DC power ports (OrangePi 3 LTS / Allwinner H6) as ASCOM Switch channels. `MaxSwitch = 3`: switch 0 = `DC3 (always on)` — the hardwired pass-through jack, read-only (`CanWrite=false`, `GetSwitch` always true, writes throw `NotImplemented`); switch 1 = `DC1` (GPIO line 118 / PD22); switch 2 = `DC2` (GPIO line 114 / PD18). Ports default to boolean on/off; **DC1/DC2 can each be switched to soft-PWM** (0–100% duty, exposed to clients like NINA as a slider) via a per-port `pwm` flag and a configurable `pwmFrequencyHz` (default **50 Hz**, range 1–100,000), using the same per-port worker-thread bit-bang approach as the ZWO ASIAIR driver. Implemented as a dedicated 4th-file protocol wrapper (`ioptron_powerbox_wrapper`) over **libgpiod v2** on `/dev/gpiochip1` (the H6 main pinctrl on the OpenAstro Armbian mainline kernel; the dead stock BSP exposed it as `gpiochip0`) — independent of the iOptron mount RS-232 protocol (`ioptron_protocol_wrapper` is untouched). The wrapper defaults controllable ports to "on" at `open()` so connecting preserves the boot state; `close()` releases the line request without driving a boolean port low, and drives any PWM port to a defined steady level (duty > 0 ⇒ on) before releasing — so disconnecting never cuts power to attached gear. PWM was confirmed on iMate hardware against a real flat panel: **frequency is the lever** — at ~1 kHz a panel's LED driver smooths the chop and gates on/off, but at the **50 Hz default** the driver cycles fully each period and the panel visibly dims (the value ZWO uses for the ASIAIR Plus). Dew heaters dim at any frequency. PWM is opt-in per port; ports stay boolean by default. The PowerBox Switch is only compiled when **libgpiod (>= 2.0)** is present at build time; on a host without it the iOptron *mount* driver still builds (telescope-only, e.g. macOS / a non-GPIO Linux box) and the switch is cleanly disabled with a CMake STATUS message rather than a hard build error.
- **iOptron iMate PowerBox Device Support** (AlpacaHTTP): router accepts `vendor=ioptron deviceType=switch` with an optional `gpioChip` override plus `pwmFrequencyHz` and a positional `ports[]` overlay (per-port `pwm`/`name`, applied onto the fixed DC3/DC1/DC2 layout — the always-on pass-through can't be PWM and its flag is ignored); config sanitization persists `gpioChip`, `pwmFrequencyHz`, and `ports` for the switch path and keeps mount connection fields from leaking into a switch config. Web UI gains iOptron as a Switch vendor, splits the iOptron config block into device-type-aware sub-sections (mount connection vs. iMate PowerBox), and adds a PWM-frequency input plus per-port PWM checkboxes for DC1/DC2 (with a note that regulated gear should stay on/off).
- **iOptron iMate PowerBox Unit Tests**: 11 test cases / 124 assertions covering defaults, device metadata, `NotConnected`/`InvalidValue` error codes, unsupported actions, per-port metadata (read-only DC3 vs. controllable DC1/DC2, names, descriptions, ranges), value range validation, state machine when disconnected, read-only/unsupported-method error codes, constructor rejection of an empty config, **PWM port metadata (analog 0–100 range, step 1, mode-aware descriptions)**, and **rejection of out-of-range PWM frequencies**. Plus an AlpacaHTTP routing test (registration, `gpioChip` + `pwmFrequencyHz` + per-port `pwm` persistence, `MaxSwitch == 3`).
- **PowerPorts.md iOptron iMate section**: promoted from stub to full setup guide for the [OpenAstro](https://github.com/open-astro/aw-flashtool) Armbian image (mainline kernel, Debian 13) — flash/self-install note, the `/dev/gpiochip1` mapping (PD22/PD18, with the gpiochip0→gpiochip1 mainline-vs-BSP explanation), GPIO access that the image preconfigures (`gpio` group + `gpiochip[0-9]*` udev rule), the fixed three-port DC3/DC1/DC2 layout, `apt install alpacabridge`, Web UI / JSON device-add, optional **per-port PWM dimming** (frequency + DC1/DC2 PWM flags, with the 50 Hz / panel-frequency note), `gpioget -c gpiochip1` verification, and disconnect behavior.
- **iOptron iMate PowerBox ConformU 4.3.0 validated** on Linux arm64 (iMate / OrangePi 3 LTS H6, OpenAstro Armbian mainline kernel, `/dev/gpiochip1`): 0 errors, 0 issues, 0 timing issues. Run against a mixed config (DC1 PWM, DC2 boolean, DC3 read-only pass-through) so all three port types were exercised in one pass; 45 timed members all within target (slowest ~31 ms vs the FAST/STANDARD targets). Results saved to `AlpacaCore/conformu/iOptron/iMate PowerBox/Linux-arm64.txt` + `.report.json`; `SUPPORTED-DRIVERS.md` Switch row updated from ⏳ to ✓.
- **ToupTek StellaVita Switch Driver** (AlpacaCore): new `SwitchDriver` exposing the StellaVita's four on-board 12V DC power ports as ASCOM Switch channels. The StellaVita is a Raspberry Pi CM4 (BCM2711); the GPIO mapping was verified on live hardware against `config.txt` (`gpio=18,10,17,4,9,11=op,dh,pu`): `MaxSwitch = 4` — Port 1=GPIO 18, Port 2=GPIO 10, Port 3=GPIO 17, Port 4=GPIO 4 on `/dev/gpiochip0` (the main pinctrl-bcm2711 bank, where the libgpiod line offset equals the BCM GPIO number). BCM GPIO 9 and 11 are deliberately **not** exposed — they power the on-board Cypress USB hub and cutting them would drop attached USB cameras/focusers. Each port defaults to boolean on/off and can be switched to **soft-PWM** (0–100% duty, exposed to clients like NINA as a slider) via a per-port `pwm` flag and a configurable `pwmFrequencyHz` (default **100 Hz** — tested best on StellaVita hardware, dims flat panels smoothly without 50 Hz flicker; range 1–100,000). Implemented as a dedicated 4th-file protocol wrapper (`touptek_powerbox_wrapper`) over **libgpiod v2**, owning all lines via a single `gpiod_line_request` with per-port worker threads for PWM channels — independent of the ToupTek camera/focuser SDK. Ports default to "on" at `open()` so connecting preserves the StellaVita's boot-high state (config.txt drives the lines high at boot); `close()` releases the request without driving a boolean port low and drives any PWM port to a defined steady level (duty > 0 ⇒ on) before releasing, so disconnecting never cuts power to attached gear. The Switch is only compiled when **libgpiod (>= 2.0)** is present; on a host without it the ToupTek camera/focuser drivers still build and the switch is cleanly disabled with a CMake STATUS message (`ALPACACORE_TOUPTEK_STELLAVITA` cache var gates the router branch and test to match).
- **ToupTek StellaVita Device Support** (AlpacaHTTP): router accepts `vendor=touptek deviceType=switch` with an optional `gpioChip` override plus `pwmFrequencyHz` and a positional `ports[]` overlay (per-port `pwm`/`name` applied onto the fixed Port 1..4 layout); config sanitization persists `gpioChip`, `pwmFrequencyHz`, and `ports` for the switch path and keeps camera/focuser index fields from leaking into a switch config. Web UI gains ToupTek as a Switch vendor, splits the ToupTek config block into device-type-aware sub-sections (camera/focuser vs. StellaVita PowerBox), and adds a GPIO-chip input, a PWM-frequency input, and a per-port table (Port 1..4 with GPIO line and PWM checkbox).
- **ToupTek StellaVita Unit Tests**: 12 test cases / 86 assertions covering defaults, device metadata, `NotConnected`/`InvalidValue` error codes, unsupported actions, per-port metadata (names, GPIO-mapped descriptions, boolean ranges, rename persistence), value range validation, state machine when disconnected, unsupported-method error codes, constructor rejection of an empty config, **PWM port metadata (analog 0–100 range, step 1, mode-aware descriptions)**, **rejection of out-of-range PWM frequencies**, and **rejection of a GPIO chip path that is not an absolute /dev/ node**.
- **PowerPorts.md StellaVita section**: promoted from stub to full setup guide — the hardware-verified `/dev/gpiochip0` mapping (Port 1..4 = BCM GPIO 18/10/17/4, with the BCM-line-offset explanation), the `gpio=18,10,17,4,9,11=op,dh,pu` config.txt directive and the **GPIO 9/11 USB-hub caveat** (do not remove), arm64 OS requirement, libgpiod deps + gpio group + udev rule, device add (Web UI / JSON), `gpioget -c gpiochip0` verification, per-port PWM dimming (frequency + the 100 Hz default / panel-frequency note), and disconnect behavior. Summary table row updated from "Pending hardware / TBD" to "Available — ConformU pending / libgpiod v2".
- **ToupTek StellaVita ConformU 4.3.0 validated** on Linux arm64 (StellaVita / Raspberry Pi CM4 BCM2711, Debian 13 Trixie, kernel 6.12.75, `/dev/gpiochip0`): 0 errors, 0 issues, 0 timing issues. Run against a mixed config (Port 1 PWM, Ports 2–4 boolean) so both the boolean and soft-PWM paths were exercised in one pass; all 50 timed members within target (slowest 16 ms vs the 100 ms FAST target). Results saved to `AlpacaCore/conformu/ToupTek/StellaVita/Linux-arm64.txt`; `SUPPORTED-DRIVERS.md` Switch section gains a ToupTek subsection with ✓.

- **Log retention auto-cleanup** (AlpacaHTTP): new `logging.retention_days` config key (default 90, 0 = forever) and `ALPACAHTTP_LOG_RETENTION_DAYS` env var. Daily files older than retention are auto-deleted on startup and again on every day-rollover inside the file sink. Today's active file is never pruned.
- **Log level persistence** (AlpacaHTTP): log level changes made via `POST/PUT /management/v1/loglevel` (i.e. the web portal toggles) now survive a server restart. The chosen level is written to `config/runtime_state.json` and reapplied on next startup, overriding `default.yaml`'s `logging.level`. Removing the state file falls back to the YAML default.
- **On-disk logging** (AlpacaHTTP): server now writes every log line to a daily file `/var/log/AlpacaBridge/alpacabridge-YYYY-MM-DD.log` in addition to stderr and the in-memory buffer. Thread-safe append sink with automatic day-rollover and a writability probe that falls back to `$XDG_STATE_HOME/AlpacaBridge/logs` (or `~/.local/state/AlpacaBridge/logs`) when the configured directory is not writable. Configurable via new `logging.directory` and `logging.file_enabled` keys in `config/default.yaml` and env vars `ALPACAHTTP_LOG_DIRECTORY` / `ALPACAHTTP_FILE_LOGGING`.
- **Log file management API** (AlpacaHTTP): three new management endpoints — `GET /management/v1/logfiles` (list daily files with size/modified time), `GET /management/v1/logfiles/{name}[?download=1]` (read inline or as attachment), `DELETE /management/v1/logfiles/{name}` (delete). Filenames are validated against the `alpacabridge-YYYY-MM-DD.log` pattern to prevent path traversal. HTTP parser extended with `DELETE` method support.
- **Web portal log file panel** (AlpacaHTTP): new "Stored log files" section under Logging shows each daily file with size and modified time, plus per-row **View** (dark-theme inline viewer), **Download**, and **Delete** (confirm-prompted) buttons. Auto-loads on page open and on the Refresh button.
- **Deb packaging** (debian/): `alpacabridge.service` adds `LogsDirectory=AlpacaBridge` so systemd creates and chowns `/var/log/AlpacaBridge` on every service start; `alpacabridge.postinst` also pre-creates the directory for non-systemd execution.
- **Dev install scripts**: `install_alpaca_service.sh` mirrors the systemd `LogsDirectory=` line and pre-creates `/var/log/AlpacaBridge` with the invoking user's ownership; `build_and_run.sh` unconditionally creates and chowns `/var/log/AlpacaBridge` on Linux so dev runs write to the standard path instead of the home-dir fallback.

### Changed
- **Completed ASIAIR brand-casing normalization** (AlpacaHTTP): the earlier `ASIair` → `ASIAIR` pass missed two user-facing router error messages (`"ASIAIR port entry requires integer 'gpio'"`, `"ASIAIR port 'gpio' must be in [0, 63]"`), the Web UI help text in `web/index.html`, and `web/app.js` comments — all now normalized. No mixed-case `ASIair` remains in source (the on-disk `conformu/ZWO/ASIair *` directory names are left as-is so `SUPPORTED-DRIVERS.md` validation links keep resolving).
- **CI cppcheck pinned to 2.17.x, built from source** (`.github/workflows/ci.yml`): the `ubuntu-24.04-arm` runner's apt cppcheck is 2.13, which classifies some checks differently from the cppcheck on a Debian 13 Trixie dev box (2.17) — e.g. `virtualCallInConstructor` is a `warning` in 2.13 but reclassified in 2.17. Since `scripts/ci_preflight.sh` runs whatever cppcheck the dev box has, that skew let the local pre-flight pass while CI failed (and vice versa). CI now builds cppcheck 2.17.1 from source (checksum-verified tarball, same pattern as the libgpiod-from-source step) so the pre-flight and CI run the same analyzer version. No source changes.
- **DriverVersion now sourced from the workspace VERSION file** (AlpacaCore): every driver's `get_driver_version()` previously returned a hardcoded `"1.0.0"`; all 19 drivers (ZWO camera/switch/focuser/filterwheel/rotator/telescope + ASIAIR Pro/Plus switches, QHY, SVBONY, Player One, ToupTek camera/focuser, iOptron, SynScan, Celestron, Bisque, Gemini, WeeWX) now return `alpacacore::kVersion`. New header `AlpacaCore/include/alpacacore/version.h` exposes `kVersion` from an `ALPACACORE_VERSION` compile definition injected from the `VERSION` file, mirroring AlpacaHTTP's existing `version.h` mechanism. The define is set with directory-scoped `add_compile_definitions(...)` in `AlpacaCore/CMakeLists.txt` (immediately after `project()`) so it reaches the core library, every per-vendor sub-library, and the test targets — a target-scoped define would not reach the vendor libs, which add the include dir directly rather than linking `alpacacore`. To bump the reported version for all drivers, edit the `VERSION` file only. The 18 per-driver unit tests now assert `get_driver_version() == alpacacore::kVersion` instead of the literal `"1.0.0"`, so they no longer need editing on a version bump. Full AlpacaCore suite green (149 tests).
- **Dead Boost.Beast / vcpkg / Windows / amd64 scaffolding removed from build glue and docs** (AlpacaBridge): follow-up to the arm64-only migration and the earlier `78e631a` Beast cleanup, which had missed several spots. The never-implemented `ALPACAHTTP_USE_BOOST_BEAST` CMake flag — which made CMake warn *"Manually-specified variables were not used by the project"* on every configure — is now gone from `run_all_tests.sh`, `install_alpaca_service.sh`, and `debian/rules` (the HTTP layer is a hand-rolled HTTP/1.1 server on POSIX sockets; it links neither Boost.Beast nor cpp-httplib). `run_all_tests.sh` also drops the entire dead Windows/`vcpkg` scaffolding (the `IS_WINDOWS_BASH`/`cygpath`/`cmd.exe` branches, the vcpkg bootstrap block, and the `x64-windows` triplet — ~45 lines), and the three now-dead `ALPACABRIDGE_ENABLE_VCPKG: "OFF"` env lines were removed from `.github/workflows/ci.yml`. Documentation corrected to match arm64-only reality: root `README.md` drops "mini PC in the observatory" and tightens to "a Debian 13 arm64 machine"; `AlpacaHTTP/README.md` drops the Beast requirement; `AGENTS.md` Gemini focuser note changes "Debian 13 x64 and ARM64" → "arm64" (only the `Linux-arm64.txt` ConformU report remains); and the Cursor rule files (`AlpacaHTTP/.cursor/rules/rules.mdc`, `AlpacaCore/.cursor/rules/{rules,driver_build,driver_test}.mdc`) now describe the hand-rolled POSIX-socket server, the real detached-worker thread model, arm64-only OS targets, the `lib/linux/armv8/` SDK layout with the hard-fail arch guard, and a single arm64 CI runner instead of an x86/macOS/Windows matrix. No functional code change — build/test behavior on Linux arm64 is identical; only the spurious CMake warning and stale guidance go away.
- **ZWO ASIAIR brand-name casing normalized** (AlpacaCore + AlpacaHTTP): user-facing "ASIair" → "ASIAIR" (ZWO's actual product branding) across both switch drivers' names, descriptions, driver info, per-port descriptions, and error/log strings, plus the Web UI Switch Type dropdown labels, the router's registration log lines, and the `SUPPORTED-DRIVERS.md` Switch section (whose driver notes were also condensed). Unit-test assertions and case names were updated to match — full AlpacaCore suite green (60 cases / 435 assertions), AlpacaHTTP routing/config/json tests green. Internal `switchType` ids (`asiair`, `asiair-plus-picm4`, `asiair-plus-rk3568`), log categories, and the gpiod consumer label are deliberately unchanged.
- **ZWO ASIair Plus (RK3568) soft-PWM default frequency aligned with ZWO's stock daemon at 50 Hz** (AlpacaCore): the wrapper's default `pwm_frequency_hz` is now 50 Hz, replacing the previous 200 Hz value that came from indoor bench tuning. Confirmed against a live stock-firmware ASIair Plus: `PWM_GPIO_GET_CONFIG` returned `period_ns = 20,000,000` (= 50 Hz) on every PWM-enabled port the stock `zwoair_imager` daemon had configured, with duties exactly matching the user's settings (dew heater 37%, flat panels 100% / 43%). The previous 200 Hz default — and the earlier 1 kHz before that — were chosen from indoor bench tests against a USB-powered LED tracing pad whose own internal touch-dimming PWM controller created cascaded-PWM artifacts that don't show up against the resistive dew heaters and DC-DC-regulated flat panels ZWO actually designed the product around. 50 Hz also matches mains frequency in China where ZWO is based, likely chosen to avoid beating against AC ripple in the 12 V input. Hardware constraint stays as documented inline: GPIO bank 4 has no hardware PWM mux on the RK3568, so soft-PWM is the only path regardless of frequency.
- **`build_and_run.sh` is now incremental by default and accelerator-aware** (AlpacaBridge): the script no longer wipes `AlpacaHTTP/build/` before each run, no longer calls `make clean` before each `make`, and no longer builds AlpacaCore standalone (that artifact was never used at runtime — AlpacaHTTP pulls AlpacaCore via `add_subdirectory`). On the first run, the script auto-installs `ccache` and `ninja-build` via `apt` if either is missing (uses the same sudo escalation as the existing udev-rule install block; configurable via `ALPACA_INSTALL_ACCELERATORS=ON|OFF`, default ON). It then auto-selects Ninja as the CMake generator and ccache as the C/C++ compiler launcher whenever those binaries are present. udev-rule + firmware + vendor `.so` installation is gated behind a hash-stamp file at `AlpacaHTTP/build/.system_setup_stamp` so the sudo block is skipped on subsequent runs when nothing has changed. CMake generator mismatch (e.g. previously-Make build dir + newly-installed Ninja) is auto-cleaned with a one-line message instead of a cryptic CMake error. `CLEAN=1 ./build_and_run.sh` is the escape hatch for forcing a full from-scratch rebuild after toolchain upgrades or genuine corruption. Measured on arm64: no-op re-run drops from ~5 min to ~5 sec (`ninja: no work to do.`); single-file edits drop from ~5 min to ~10–30 sec; first clean build with Ninja+ccache drops from 5–10 min to ~4 min.
- **`/management/v1/logs` now reads today's daily file from disk** (AlpacaHTTP): the legacy in-memory log buffer (`g_log_history` deque, `get_log_history_text`) has been removed entirely. The endpoint and the "Download Logs" button now stream today's `alpacabridge-YYYY-MM-DD.log` straight from disk. Side effect: log content survives server restarts without any in-memory replay. `util::read_log_file` enforces a 10 MiB per-request cap to protect against runaway-volume days.
- **Alpaca-style log management endpoints now return HTTP 200 on driver errors** (AlpacaHTTP): the new `/management/v1/logfiles` family was returning HTTP 4xx alongside Alpaca error envelopes, which violated the project's transport contract. All error branches now return HTTP 200 with `ErrorNumber != 0` in the body.
- **Log-level persistence failures no longer block the API** (AlpacaHTTP): if `config/runtime_state.json` can't be written (read-only filesystem, etc.), the level change still succeeds in-memory and the persistence failure is downgraded to a WARNING log line.
- **Web portal log viewer is size-guarded** (AlpacaHTTP/web): inline view refuses to render files > 5 MiB and suggests Download instead. List loader now respects Alpaca `ErrorNumber` and surfaces server-side errors instead of silently showing an empty state. Deprecated `word-break: break-word` replaced with `overflow-wrap: anywhere`.
- **build_and_run.sh log-dir setup is best-effort** (AlpacaBridge): when `sudo` is unavailable the script now warns and continues instead of hard-failing; the server's fallback log directory handles the no-sudo case.
- **install_alpaca_service.sh `update` refreshes the unit file** (AlpacaBridge): the update path now calls `write_service_unit` so existing installs pick up `LogsDirectory=` and any future unit-file changes.
- **Internal error-code handling unified and made deterministic** (AlpacaCore + AlpacaHTTP): `alpacacore::AlpacaError` is now the single source of truth for ASCOM Alpaca error numbers; `alpacahttp::util::ErrorCode` aliases its values (constexpr references) so the two layers can no longer drift, and a cluster of dead non-standard pseudo-codes in the 0x501–0x506 driver-specific range (`Slaved`/`Parked`/`InvalidWhileSlewing`/`NotAtHome`/`InvalidOperationException`(2), the `NotSupported` 0x500 alias, and the unused `DRIVER_NOT_READY`/`NOT_SAFE`) were removed along with the `map_error_code` cases that only existed to remap them back to standard codes. Separately, the router's 92 parameter-validation failures now throw `AlpacaException` carrying an explicit `InvalidValue` (0x401) code via a new `throw_invalid_value()` helper, instead of a bare `std::runtime_error` whose ASCOM `ErrorNumber` was *inferred* by substring-matching the exception message — the wire error number is now deterministic and no longer changes silently if a message is reworded. No change to the error numbers clients see today.

### Removed
- **amd64/x86_64 architecture support** (AlpacaBridge): AlpacaBridge is now Linux **arm64-only**. CMake, `debian/control` (`Architecture: arm64`), `debian/rules`, `build_and_run.sh`, and `install_alpaca_service.sh` hard-fail on non-arm64 hosts; per-vendor `CMakeLists.txt` files (ZWO/QHY/SVBONY/ToupTek/Player One) drop their x86_64 branches. ~141 MB of x86_64 vendor SDK binaries removed from the tree: `QHY/sdk_linux64_25.09.29/`, `ZWO/{ASI_Camera_SDK,EAF,EFW,CAA}/lib/x64/`, `SVBONY/lib/x64/`, `ToupTek/toupcamsdk.20260128/linux/x64/`, `PlayerOne/.../lib/x64/`. All 24 amd64/x64 ConformU reports under `AlpacaCore/conformu/` deleted; only `Linux-arm64*.txt` reports remain. `SUPPORTED-DRIVERS.md` driver tables drop the Linux (x64) column. AGENTS.md, `docs/architecture.md`, `docs/development.md`, `docs/troubleshooting.md`, and the `.claude/commands/{submit-pr,commit,driver-build}.md` skill prompts updated to reflect arm64-only targeting. Rationale: maintaining and ConformU-validating two architectures was burning time with no known amd64 users.
- **In-memory log retention toggle** (AlpacaHTTP): removed the "Log history retention" web UI control, the `/management/v1/loghistory` management endpoint, the `logging.history_limit` YAML key, and the `ALPACAHTTP_LOG_HISTORY_LIMIT` env var. The in-memory buffer is also gone in favor of reading today's daily file directly from disk — durable history lives in the per-day on-disk files.
- **Orphaned ConformU JSON report** (AlpacaCore): deleted `AlpacaCore/conformu/ZWO/ASIair Pro/Linux-arm64.report.json` to match the recent `/conformu` policy change — the text log is now the single source of truth for ConformU validation, and the parallel JSON report was a drift risk. Going forward `/conformu` no longer generates or saves `*.report.json` files.
- **Non-standard `Shutter` device type** (AlpacaCore + AlpacaHTTP): removed the standalone `Shutter` device type — it is not one of the ten ASCOM Alpaca device types (shutter control belongs to the **Dome** interface: `OpenShutter`/`CloseShutter`/`ShutterStatus`). It existed only as unused scaffolding (interface header, empty `.cpp`, `DeviceType::Shutter` enum, and full router dispatch) with no concrete driver, no registration path, and nothing in config, yet its header claimed to follow a nonexistent "ASCOM Alpaca Shutter API specification". A non-standard device type would surface in `/management/v1/configureddevices` as `DeviceType: "Shutter"`, which standard clients (NINA, ConformU) cannot consume. Camera `HasShutter`, Dome `CanSetShutter`, and vendor mechanical-shutter code are unaffected.

### Fixed
- **GPIO power-switch driver hardening (StellaVita, iMate PowerBox, ASIAIR Pro / Plus Pi CM4, ASIAIR Plus RK3568, ZWO dew heater)** (AlpacaCore + AlpacaHTTP): a sweep of robustness fixes applied consistently across every Switch driver. (1) `SetSwitchValue` now rejects a non-finite (`NaN`/`Inf`) value from the HTTP API with `InvalidValue` before it reaches `std::lround`, which is undefined behaviour on non-finite input — fixed in the StellaVita, iMate, ASIAIR Pro/Plus, RK3568, and ZWO dew-heater drivers. (2) The libgpiod soft-PWM wrappers (StellaVita, iMate, ASIAIR) now serialise **every** `gpiod_line_request_set_value` call (boolean writes, per-port PWM worker writes, and close-time settle writes) through a dedicated `io_mutex_` held only around the ioctl — closing a thread-safety gap where a mixed boolean+PWM config issued concurrent writes on the same `gpiod_line_request` from a worker thread and a client thread (libgpiod v2 does not document the request as safe for concurrent writers). The RK3568 wrapper already serialised all ioctls through its existing mutex and was unaffected. (3) The wrappers now validate that the configured `gpioChip` is an absolute `/dev/` device node at construction (rejecting `""`, a relative path, or a bare `gpiochip0` with `InvalidValue`) instead of surfacing an opaque `gpiod_chip_open` failure later. (4) The close-time PWM settle writes now check their return value and `WARN`-log a failure instead of releasing the line in a silently-indeterminate state. (5) The web UI now always persists the per-port `ports` overlay and `pwmFrequencyHz` for the StellaVita and iMate switches, so a custom PWM frequency or per-port config survives a re-save with every PWM box un-ticked. (6) The read/write members (`GetSwitch`, `GetSwitchValue`, `GetStateChangeComplete`, etc.) now validate the switch ID **before** the connection check across all five drivers, so an out-of-range ID throws `InvalidValue` regardless of connection state per the ASCOM Switch spec (previously a disconnected `GetSwitch(-1)` returned `NotConnected`). Added unit tests for the GPIO-chip-path validation and the ID-before-connection ordering on all the switch drivers.
- **Alpaca `Value` no longer guessed by re-parsing strings** (AlpacaHTTP): `to_json(AlpacaResponse)` used to take any string `Value` and try `nlohmann::json::parse()` on it, substituting the parsed result if it succeeded. That corrupted legitimate string-typed ASCOM properties whose text happens to be valid JSON — a serial/sensor name like `"12345"` became the number `12345`, `"true"` became a boolean, etc. (wrong type on the wire). It also forced structured endpoints (`configureddevices`, `description`, `apiversions`, log payloads) to `.dump()` an array/object into a string purely so `to_json` would parse it back. `AlpacaResponse::value` is now `std::optional<nlohmann::json>` (was `optional<variant<bool,int32,double,string>>`), so handlers assign scalars, strings, arrays, and objects directly and `to_json` emits them verbatim — strings stay strings, structured values stay structured. All sixteen handlers that previously `.dump()`-ed a payload into a string drop the `.dump()`: the seven management handlers (`description`, `configureddevices`, `apiversions`, configuredevice info, `loglevel`, logfiles ×2) and the nine device-API array handlers (`SupportedActions`, `DeviceState`, `AxisRates` ×2, camera `Gains`/`Offsets`/`ReadoutModes`, filter `Names`/`FocusOffsets`) — the latter were caught by ConformU, which rejected the stringified arrays with "The JSON value could not be converted to IList`<String>`". New `test_json` cases lock in that a JSON-looking string `Value` stays a string and structured values round-trip as JSON; a `test_routing` case asserts `SupportedActions` serializes as a JSON array.
- **ASIAIR Plus (Pi CM4) switch reported itself as a "Pro"** (AlpacaCore + AlpacaHTTP): the Pi CM4 ASIAIR Plus shares the Pi 4 ASIAIR Pro's on-board GPIO wiring and reuses the same libgpiod switch driver, but the driver hard-coded "ASIAIR Pro" in `get_name()`/`get_description()`/`get_driver_info()`, so a CM4 Plus identified as a Pro in ConformU and to clients. `AsiairSwitchConfig` now carries a `model_name` (default `"ASIAIR Pro"`) that the three strings interpolate, and the router sets it to `"ASIAIR Plus (Pi CM4)"` for `switchType: asiair-plus-picm4`. Added a unit test covering the Plus label. (The committed CM4 Plus ConformU log still shows the old "Pro" strings until re-run on hardware.)
- **AlpacaHTTP hand-rolled tests silently did nothing under `-DNDEBUG`** (AlpacaHTTP/tests): `test_routing`, `test_json`, `test_config`, and `test_discovery` drove their checks — including side-effecting calls like `request.parse(...)` — through `assert()`. In a Release/`NDEBUG` build (which is what `debian/rules` and the released `.deb` use) `assert()` is stripped, so those calls never ran and the tests validated nothing (and `test_routing` then crashed on the resulting empty state). Replaced `assert()` with a new always-on `EXPECT()` macro (`tests/test_assert.h`) that evaluates its expression exactly once and aborts on failure regardless of build type. Full AlpacaHTTP suite now passes in a Release build (154/154). Server code was never affected — the bug was entirely in the tests.
- **Static-analysis cleanup of pre-existing cppcheck/clang-tidy findings** (AlpacaCore + AlpacaHTTP): cleared the findings the diff-scoped CI gates surface in files this work touches (issue #64). The one real bug: the iOptron telescope destructor called `set_connected(false)` directly, so a throw during teardown would propagate out of the implicitly-`noexcept` destructor and call `std::terminate()` — now wrapped in a best-effort `try/catch` whose handler logs (an empty handler trips `bugprone-empty-catch`). Both the iOptron and Celestron destructors now call `set_connected` with explicit class qualification so it binds statically — this is the intended behavior in a destructor and clears `virtualCallInConstructor` (cppcheck) and `clang-analyzer-optin.cplusplus.VirtualCall`. The four `identicalInnerCondition` warnings (Celestron + iOptron telescope drivers) are false positives — the inner `if (!site_info_valid_)` re-check detects a *failed* `ensure_site_info_cached_locked()` populate, which cppcheck can't see through the const call — and were silenced with documented `// cppcheck-suppress` comments. The router's `is_expected_not_implemented` `||` chain dropped its `PropertyNotImplemented`/`MethodNotImplemented` arms (both alias the same 0x400 code as `NotImplemented`; cppcheck flagged the redundancy as `knownConditionTrueFalse`). Remaining mechanical perf/style fixes, no behavior change: `x = x.substr(0,n)` → `x.resize(n)` (Celestron/SynScan protocol wrappers, HTTP router), old-style `(struct sockaddr*)` casts → `reinterpret_cast` (discovery + HTTP server), `strip_status_prefix` takes `const std::string&`, a redundant `.c_str()` dropped, and `find("web/") != 0` → `!starts_with("web/")`. **Deferred:** the ZWO ASIair `returnByReference` findings (`device_path()`/`gpio_chip_path()` → `const std::string&`) are left for a follow-up — those files `#include <gpiod.h>`/`<pwm_gpio.h>`, and the clang-tidy gate configures with `ALPACACORE_ENABLE_ALL_VENDORS=OFF`, so any edit to them fails the gate on unresolved vendor headers (libgpiod v1 in CI vs the v2 API in-tree). The ZWO mount `useInitializationList` fix landed (that file pulls in no SDK headers).
- **Server startup self-deadlock when preferred log directory is unwritable** (AlpacaHTTP): `configure_log_directory` in `util/logging_adapter.cpp` was emitting the `"Log directory '...' not writable; using fallback '...'"` (and the "No writable log directory available; file logging disabled") warnings *while still holding* `g_file_mutex`. The warning routes through the registered log sink → `write_to_file`, which re-acquires the same non-recursive `std::mutex` on the same thread — instant self-deadlock, manifesting as the server hanging single-threaded on `futex_wait` immediately after the WARN line is printed. Any fresh install where `/var/log/AlpacaBridge` does not exist or is not writable hit this (it's the exact scenario `build_and_run.sh` documents with its `"Note: skipping /var/log/AlpacaBridge setup (sudo unavailable)"` log line — no `.deb`, no passwordless sudo, no log dir, no listener). Fix: capture the warning text into a local string under the lock, release the lock at scope exit, then emit `log_warning` from the now-lock-free section. Verified by running with `ALPACAHTTP_LOG_DIRECTORY` pointed at a non-existent `/var/log/*` path — server now binds within 1 second and serves `/management/v1/configureddevices` correctly.
- **Alpaca Discovery** (AlpacaHTTP): UDP discovery listener on port 32227 no longer tears itself down when the multicast group join fails. The ASCOM Alpaca discovery protocol primarily uses UDP broadcast to `255.255.255.255:32227` (which a socket bound to `INADDR_ANY:32227` already receives), so multicast-join failure is now logged as a warning and discovery continues serving broadcast/unicast probes. Fixes NINA "Discover Servers" returning zero results when AlpacaBridge runs on an RPi acting as its own Wi-Fi access point (NetworkManager `ipv4.method shared`), where `wlan0` has no default multicast route and `IP_ADD_MEMBERSHIP` fails. Externally-routed LAN setups are unaffected.
- **Alpaca parameter names now case-insensitive for every client** (AlpacaHTTP): the ASCOM Alpaca API definition states "Parameter names are not case sensitive, so clients and drivers should be prepared for parameter names to be supplied … with any casing." The server previously enforced case-sensitive matching *only* when the `User-Agent` was ConformU and accepted any casing otherwise — so conformance-test behavior differed from production — and PUT form-body parameter names were effectively case-sensitive for every client. Query and form parameter lookups are now consistently case-insensitive and the `User-Agent`-gated strict path was deleted, so test behavior equals production behavior. Added vendor-free regression tests.
- **HTTP 400 for requests the device cannot interpret** (AlpacaHTTP): per the Alpaca spec ("HTTP 400 indicates that the device could not interpret the request e.g. an invalid device number or misspelt device type"), an unknown device type, unknown method, or unregistered device number now returns HTTP **400** instead of 404. Genuinely unroutable URLs still return 404, and driver exceptions still return HTTP 200 with `ErrorNumber` (unchanged). Added regression tests for the three 400 cases.

## [1.0.3] - 2026-05-07

### Added
- **ToupTek AAF Focuser Driver** (AlpacaCore): new `FocuserDriver` implementation for ToupTek Astro Auto Focuser devices, sharing the existing `toupcamsdk.20260128/` SDK with the camera driver.
  - SDK wrapper extended with `enumerate_focusers()` (filters `Toupcam_EnumV2` results by `TOUPCAM_FLAG_AUTOFOCUSER`), `open_focuser_by_id()`, `close_focuser()`, and generic `aaf_set` / `aaf_get` / `aaf_range` helpers.
  - `ToupAAF` action constants exposed in the wrapper header so driver code does not include `toupcam.h` directly.
  - Async connect/disconnect, RAII cleanup, mutex-guarded device state. Capabilities: absolute positioning, halt, max-step query, on-board temperature (°C from tenths-of-°C firmware reading), backlash range, reverse direction. `StepSize` reports `PropertyNotImplemented`; `TempCompAvailable` is false (no AAF temp-comp action).
  - `Toupcam_AAF` argument convention documented: SET = `(action, value, nullptr)`, GET = `(action, 0, &out)`, RANGE = `(RANGEMAX, GETxxx, &out)`.
  - ConformU 4.3.0 validated for **ToupTek AAF** on Linux x64 and arm64 with 0 errors and 0 issues.
- **ToupTek AAF Device Support** (AlpacaHTTP): router registration for `vendor=touptek deviceType=focuser` with `focuserIndex` / `focuserId` config; web UI extended so ToupTek is selectable for Camera and Focuser, with conditional camera/focuser fields and config persistence; routing test covers focuser registration, config round-trip, and removal.
- **ToupTek Focuser Unit Tests**: 10 test cases, 53 assertions covering Defaults, Device metadata, Not-connected error code, Unsupported actions/methods, Absolute focuser semantics, Value range validation, State machine, and create-by-index/create-by-id factories.
- **ASCOM Contract Tests** (AlpacaCore): added `require_alpaca_error` helper and ASCOM error code verification across all 12 driver test files (121 test cases, 897 assertions). Tests verify specific ASCOM error codes (NotConnected, InvalidValue, NotImplemented, ActionNotImplemented, ValueNotSet) without requiring hardware, replicating key ConformU checks at the unit test level.
  - Camera drivers (ZWO, QHY, SVBONY, ToupTek, Player One): ASCOM error codes and CameraState machine contracts
  - Telescope drivers (iOptron, SynScan, Celestron, Bisque, ZWO): ASCOM error codes, target coordinate persistence, site property validation, telescope property contracts
  - Switch (ZWO), ObservingConditions (WeeWX): ASCOM error codes for disconnected operations
- **Documentation** (docs/): consolidated `AlpacaCore/docs/` and root `DEVELOPMENT.md` into `docs/development.md`, `docs/architecture.md`, and `docs/troubleshooting.md`. Updated test requirements to 8 cases / 30+ assertions with ConformU-aligned contract test patterns.
- **Claude Code Skills** (.claude/commands/): added `/commit`, `/submit-pr`, and `/driver-build` slash commands for guided development workflows
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
- **Celestron Telescope Driver** (AlpacaCore)
  - ConformU validated for **Celestron CGX-L** on Linux x64 with 0 errors and 0 issues; arm64 validation pending.

### Changed
- **SynScan Telescope Driver** (AlpacaCore)
  - Auto-detection of SynScan mounts: `connectionType: "auto"` scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for SynScan hand controllers, probes each port with a firmware version query, and connects to the first responding mount. No manual port configuration required.
  - Implemented pulse guiding via software-timed variable-rate slew (SynScan V3/V4 protocol has no hardware pulse guide command). Driver issues a variable-rate axis slew at the guide rate, sleeps for the requested duration, then stops the axis and restores sidereal tracking.
  - GEM pier-side DEC direction flip: DEC motor direction is inverted when the mount's pointing state is 'W' (west), matching the physical axis reversal on German equatorial mounts.
  - Position override accumulation for pulse guide coordinate reporting: instead of reading back noisy mount positions after tiny guide pulses, the driver accumulates expected `rate × duration` deltas directly into the target coordinate frame. All consecutive pulse guide directions (N/S/E/W) operate in the same coordinate baseline, eliminating drift between reads.
  - RA tracking restoration after pulse guide: the stop thread re-issues `set_tracking_mode()` after stopping an RA-axis pulse to counteract the variable-rate stop command killing sidereal tracking.
  - `IsPulseGuiding` now returns actual status (time-based tracking of pulse guide end time plus completion delay) instead of always returning false.
  - ConformU 4.3.0 validated for **Sky-Watcher HEQ5 PRO** on Linux x64 with 0 errors and 0 issues across all four pulse guide directions at declinations -9, +9, -3, +3.
- **iOptron Telescope Driver** (AlpacaCore)
  - Auto-detection of iOptron mounts over serial: `connectionType: "auto"` scans `/dev/serial/by-id/` and `/dev/ttyUSB*` for Prolific/FTDI/CP210x/Silicon Labs USB-serial adapters, probes each port with `:MountInfo#`, and connects to the first responding mount. No manual port configuration required.
  - Network auto-discovery of iOptron mounts over Wi-Fi: when using Network connection type, the driver probes well-known iOptron Wi-Fi module addresses (`10.10.100.254`, `10.10.100.1`, `192.168.100.1`) on ports 8899 and 4030, then the default gateway on each local interface, and finally scans all hosts on local subnets (up to /24) with parallel non-blocking TCP connect probes. Each candidate is verified with `:MountInfo#` before acceptance.
  - Mount model identification via `:MountInfo#` query using the INDI v3 model code table (60+ models). `get_name()` now returns the detected model (e.g., "iOptron HEM27") instead of the generic "iOptron Telescope".
  - `iOptronPortInfo` struct and `enumerate_ioptron_ports()` / `model_code_to_name()` utility functions in the protocol wrapper.
  - Default baud rate changed from 9600 to 115200 for serial connections, matching the iOptron RS-232 v3.10 protocol specification.
  - `get_mount_info()` now populates `model_name` and `has_encoder` fields from the `:MountInfo#` response.
  - `SideOfPier` now computed from hour angle (LST − RA) per the ASCOM convention (`pierEast` for HA ≥ 0, `pierWest` for HA < 0) instead of returning the raw physical pier side from the mount, which does not match ASCOM semantics when tracking past the meridian.
  - `SlewToCoordinates` and `SlewToCoordinatesAsync` input validation now runs before sync offset subtraction, preventing `normalize_ra_hours()` from converting invalid RA values (e.g., -1, 25) into valid ones.
  - Slew target coordinates split into ASCOM-facing values (for `TargetRightAscension`/`TargetDeclination` readback) and physical values (with sync offset applied, for actual mount dispatch), fixing ConformU `SlewToTarget` DEC errors.
  - Pulse guide cross-axis hold grace period increased from 200 ms to 2000 ms, ensuring the frozen DEC value persists long enough for clients to read position after `IsPulseGuiding` returns false.
  - ConformU 4.3.0 validated for **iOptron HEM27** on Linux x64 and arm64 with 0 errors and 0 issues over both USB and Wi-Fi.
  - Wi-Fi reliability: blind commands (`:ST1#`, `:SR9#`, `:qR#`, `:mw#`, etc.) now drain stale TCP acknowledgment bytes via non-blocking `poll()`/`select()` after each send, preventing buffer accumulation that overwhelmed the mount's Wi-Fi module and caused GEP/GLS timeouts.
  - `IsPulseGuiding` response time improved from ~150 ms to sub-millisecond on Wi-Fi by replacing the main mutex lock with lock-free `std::atomic` fields (`pulse_guiding_active_`, `pulse_guiding_end_ns_`), meeting the ConformU fast response target over high-latency links.
  - Removed position tolerance shortcut from `get_slewing()` that prematurely declared slews complete while the mount was still physically moving (GLS status=2). The mount's own status register is now trusted, and the settle loop in `wait_for_slew_completion` handles final position convergence.
  - `MoveAxis` tertiary axis (axis=2) now throws `InvalidValue` instead of `MethodNotImplemented`, matching ConformU 4.3.0 expectations.
  - Settle loop rewritten from capped iteration count to deadline-based position stability detection with 3 consecutive stable reads within 30 arcseconds threshold.
- **iOptron Device Support** (AlpacaHTTP)
  - Web UI: iOptron connection type selector with Auto-Detect (default), Serial, and Network options, matching the Celestron configuration pattern.
  - All iOptron web UI element IDs prefixed with `ioptron-` to avoid collisions with other vendor config sections.
- **Celestron Telescope Driver** (AlpacaCore)
  - Pulse guide rewritten to use native MC_AUX_GUIDE (0x26) hardware command instead of software-timed MoveAxis + sync. The firmware times the pulse internally — no sleep, encoder snapshotting, or sync_ra_dec_raw calls required.
  - `SideOfPier` now reports actual pier side via the HC `p` command (`W` → pierWest, `E` → pierEast) instead of always returning -1 (unknown).
  - `IsPulseGuiding` now returns actual status (time-based tracking of pulse guide end time plus completion delay) instead of always returning false.
  - Added pulse guide position hold/correction pattern: cross-axis is frozen at pre-pulse value during the guide window, active axis returns computed `baseline + (rate × duration)` as a one-shot correction. Fixes ConformU tolerance failures at high declinations (DEC > 80°) where cos(DEC) amplification causes geometric noise.
  - Pier-safety gate relaxed to accept either a successful `SyncToCoordinates` in the current driver session **or** HC-reported alignment (`J` command), bringing behavior in line with INDI/INDIGO. HC workflow: power on → Switch Position → Location → Last Alignment → "CGX-L Ready".
  - Post-slew tracking restoration now re-issues the top-level `T` set-tracking-mode command rather than a per-axis variable-rate passthrough, keeping the HC's internal tracking state coherent with the LCD readout on CGX-L fw 7.18.
  - Added adaptive RA slew offset (matches INDI's `SlewOffsetRa`): driver learns a running average of RA undershoot and pre-biases subsequent slews to compensate for the CGX-L's no-tracking-during-goto behavior.
  - `SiteLatitude`, `SiteLongitude`, and `UTCDate` writes are silently skipped when the mount is aligned (log warn, no protocol call, return success), matching INDI's UpdateLocation/UpdateTime pattern — preserves ConformU property round-trip tests while protecting HC alignment models.
  - Documented required firmware (HC GEM 5.35.3179, MC 7.18.5020) and HC startup procedure in `SUPPORTED-DRIVERS.md`.
- **NexStar Protocol Reference** (AlpacaCore)
  - Added HC `p` command (Get Pier Side) for GEM mounts, with ASCOM mapping notes.
  - Added model IDs for CGX (14), CGX-L (20), and Evolution (22).
  - Added implementation notes: RA slew offset compensation, post-slew tracking restoration via `T` command, and pulse guide position hold/correction pattern.
- **External SDK directory**: renamed `AlpacaCore/external/Player One/` (space) → `AlpacaCore/external/PlayerOne/` (no space) so `debian/rules` Makefile variables and shell install scripts don't break on the embedded whitespace.
- **Supported Drivers Documentation**: updated iOptron Driver Notes with auto-detection, mount identification, tested firmware details (HEM27, V240121/V241201), Wi-Fi reliability notes, and ConformU validation status (USB and Wi-Fi).
- **Supported Drivers Documentation**: added Player One section between QHY and SVBONY with Ceres 462M entry, ConformU link, and driver notes (SDK version, tested model, cooling gating, dew-heater not-wired status, pulse guiding mechanism).
- **Supported Drivers Documentation**: alphabetized vendors within the Camera Drivers section (Player One now precedes QHY).
- **Web UI** (AlpacaHTTP): alphabetized vendor dropdown options and camera configuration `<div>` blocks in `index.html` so the Focuser group shows Gemini before ZWO and the Camera group shows Player One, QHY, SVBONY, ToupTek, ZWO in order.
- **Web UI** (AlpacaHTTP): fixed vendor dropdown alphabetical order for Celestron telescope mount.
- **Build System** (AlpacaHTTP): removed unused Boost.Beast option and empty `session.cpp` placeholder
- **Platform Documentation**: added Raspberry Pi 3B+ to supported arm64 targets

### Fixed
- **iOptron Telescope Driver** (AlpacaCore)
  - Fixed `:MountInfo#` response parsing: iOptron returns exactly 4 ASCII digit bytes with no `#` terminator, but the driver was waiting for a `#` and timing out silently. Changed to idle-timeout read mode (`require_hash_terminator=false`).
  - Fixed model code table: iOptron reassigned model codes in the v3 protocol (e.g., code `0025` is HEM27, not CEM25). Replaced the stale Indigo-derived table with the current INDI v3 driver's authoritative mapping.
  - Fixed stale serial buffer bytes contaminating `:MS1#`/`:MS2#` slew responses (e.g., `"1111"` instead of `"1"`). Added `flush_input()` (via `tcflush`/`PurgeComm`) before issuing slew commands.
  - Fixed Wi-Fi timeout cascade during MoveAxis testing: rapid-fire blind commands accumulated stale acknowledgment bytes in the TCP receive buffer, overwhelming the mount's Wi-Fi module and causing subsequent GEP/GLS queries to timeout. Added `drain_network_stale()` using `poll()` (Linux) / `select()` (Windows) to consume pending bytes after each blind command.
  - Fixed `SlewToTarget` DEC accuracy on Wi-Fi: `get_slewing()` had a position tolerance shortcut (60 arcseconds) that overrode the mount's GLS status register, declaring slews complete while the mount was still physically moving. ConformU measured 53.8" error. Removed the shortcut — slew completion now relies solely on the mount reporting stopped/tracking status.
  - Fixed `strip_status_prefix()` to handle stale `0`/`1` bytes that accumulate before GEP position responses over TCP, finding the first `+`/`-` sign to locate the actual data start.
- **Device Persistence** (AlpacaHTTP)
  - Fixed stale device entries persisting across restarts: a single device failing to load on startup no longer prevents other devices from loading (individual try/catch per device).
  - Fixed inability to remove failed devices: `handle_remove_device` now checks both the runtime registry and the persisted device list, so devices that failed to register can still be deleted.
  - Fixed failed devices being invisible in the web UI: `handle_configured_devices` now includes persisted-but-unregistered devices marked with `LoadError: true`, displayed with a warning icon and red styling.
- **Celestron Telescope Driver** (AlpacaCore)
  - Fixed SideOfPier race condition in RA offset learning: async slew lambda now captures target RA at dispatch time so back-to-back slews don't corrupt the running-average residual.

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

### Changed
- **Documentation** (AlpacaBridge)
  - README rewritten around installation via the [apt.openastro.net](https://apt.openastro.net) APT repository now that packaged releases are public.
  - Development workflow (build scripts, CMake flags, source-install, custom-driver guidance) moved to a new `DEVELOPMENT.md`.
  - Added Wiki link to README.
- **Web UI** (AlpacaHTTP)
  - Bisque vendor option temporarily hidden in the device configuration dropdown until the driver ships.
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

