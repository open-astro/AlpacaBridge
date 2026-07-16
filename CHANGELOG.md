# AlpacaBridge Changelog

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

All notable changes to AlpacaBridge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

AlpacaBridge is a workspace that combines [AlpacaCore](AlpacaCore/README.md) and [AlpacaHTTP](AlpacaHTTP/README.md).

## [UNRELEASED] - 3.0.1

Full-codebase audit sweep (AUDIT.MD, 2026-07-11): all Critical/High/Medium findings and the tractable Low findings resolved. Network-authentication items were out of scope by design — ASCOM Alpaca has no auth model.

### Security
- **HTTP: path-traversal / arbitrary file read in static web serving** (audit C1): `handle_static_file` built filesystem paths by string concatenation, so `GET /web/../../../../etc/passwd` returned any file the process could read. Requests containing `..` segments are now rejected outright and every resolved path is canonicalized (`weakly_canonical`) and confined under the canonical web root, which also blocks symlink escapes.
- **HTTP: slowloris / resource-exhaustion hardening** (audit H6, M1–M3): accepted sockets now carry 30 s send/receive timeouts (idle connections can no longer park worker threads forever); requests are read with a proper accumulation loop (64 KB header cap → 431, body read to exactly `Content-Length`) instead of a single 8 KB `recv`; `Content-Length` is validated and capped at 10 MB (→ 413/400) instead of being fed unchecked into `resize()`; responses are sent with a short-write-safe `send_all` loop with `MSG_NOSIGNAL` instead of a single unchecked `send`.
- **systemd sandboxing for the packaged service**: `NoNewPrivileges`, `PrivateTmp`, `ProtectHome`, `ProtectSystem=strict` (+`ReadWritePaths`), `ProtectKernelTunables/Modules`, `RestrictSUIDSGID`; the over-privileged `input` group enrollment is removed (ZWO HID access comes from the packaged udev rules). Runtime state moves from `/etc/alpacabridge` to `/var/lib/alpacabridge` (`WorkingDirectory` + `StateDirectory`; postinst migrates existing registered-device state, never overwriting).
- **Web UI: latent XSS closed** (audit low): `escapeHtml` now escapes quotes, and the device-delete inline `onclick` is replaced with a data-attribute `addEventListener` handler.

### Fixed
- **Detached-thread use-after-free / `std::terminate` family across vendor drivers** (audit C2, C3, H1–H5): every remaining detached thread that captured `this` is converted to a joinable member thread with a cancel flag + condition variable, cancelled and joined on disconnect and in the destructor — ZWO telescope teardown and async-GOTO threads (C2/H1), Celestron/SynScan/iOptron async-slew and pulse threads (H4), QHY cooler-off and pulse-guide timers (H2/H3, using Player One's `shared_ptr`-flag pattern). The Celestron async-slew tail is additionally wrapped in try/catch so a disconnect-during-slew can no longer throw out of the thread and `std::terminate` the whole server (C3). QHY's temp/telemetry thread starters are serialized so a double-start can no longer destroy a joinable thread (H5). `grep '\.detach()'` across all vendor drivers now finds only the documented no-`this` timer.
- **QHY: disconnect mid-exposure freed the SDK handle under a live `GetQHYCCDSingleFrame`** (audit C4): disconnect now cancels the exposure (waking the blocking read), joins the exposure worker under the lifecycle mutex, and only then closes the camera — the same order as every sibling driver and the destructor. Also adds the sibling drivers' exposure watchdog (deadline = exposure + 15 s margin) so a hung SDK read reports `Failed` instead of `Exposing` forever.
- **Connect-side ref-counted open leaks** (audit H8, H9): ZWO and SVBONY cameras now guard the entire post-open init with close-and-rethrow (ToupTek's shape), so a throwing init call no longer leaks the open permanently; ZWO additionally treats a missing camera serial number as non-fatal — cameras without one could previously never connect at all.
- **SDK wrapper mutex held across blocking image downloads** (audit H10, H11): the SVBONY wrapper no longer holds the singleton mutex across `SVBGetVideoData` (or the duration-blocking `SVBPulseGuide`), and the ZWO wrapper no longer holds its global mutex across `ASIGetDataAfterExp` — abort/status/other-device calls no longer stall behind a multi-second download.
- **SynScan: binary protocol payloads were read through the text-framing path** (audit H12): latitude/hour/model bytes of value 10/13/35 were stripped or truncated the frame, corrupting site/time/pointing state. Celestron's `binary_bytes` exact-read phase is ported; all binary commands (`w`,`h`,`m`,`t`,`J`,`p`) now read exact byte counts.
- **SynScan: pulse-guide stop failure could leave an axis slewing indefinitely** (audit H13): the stop is retried 3×; on final failure the driver logs a runaway warning and reports the axis as slewing instead of silently swallowing the error.
- **Network mounts: blocking TCP `connect()` under the driver mutex** (audit H14): Bisque/Celestron/SynScan/iOptron now use the non-blocking connect + `poll` (7 s) pattern, so an unreachable mount IP no longer wedges the driver for the ~2 min kernel timeout. Also replaces obsolete `gethostbyname` with `getaddrinfo` at all four sites, and the singleton protocol wrappers now refuse a second concurrent connect with a clear error instead of silently stealing the first device's connection.
- **Celestron/SynScan: sync slews & park held the driver mutex up to 120 s** (audit M19): converted to Bisque's unlock/sleep/relock wait with a connection re-check after every relock (and Bisque itself gained the re-check); property GETs and disconnect no longer block for the whole slew. Also fixes a guaranteed self-deadlock where `sync_time_on_connect` re-locked the driver mutex via the public `set_utc_date()`.
- **ZWO telescope/rotator/switch/camera fixes** (audit M9–M13): `move_axis`/`slew_to_target_async` no longer leave the poll thread paused forever after a throwing protocol call; a latitude-only site set no longer writes `longitude=0` to the mount; `get_full_well_capacity` drops a spurious ×1000; `pulse_guiding_end_` and rotator `sync_offset_` reads/writes are brought fully under the driver mutex; the switch's `dew_caps_or_throw` returns by value instead of a reference into a lock-guarded optional (the #116 camera fix, swept).
- **Camera exposure integrity sweep** (audit M14–M18): ToupTek RAW8-only cameras no longer read frames as 16-bit with a doubled row pitch (every frame was garbled); SVBONY/PlayerOne/QHY setters (gain/offset/ROI/bin/readout/temp target where applicable) now reject mid-exposure writes with `InvalidOperation` under the publishing lock (ToupTek's `ensure_not_exposing` swept everywhere); SVBONY re-marks ROI/speed dirty flags on apply failure instead of bricking all later exposures; SVBONY/PlayerOne/QHY clear cached capabilities on disconnect and range getters throw `NotConnected` instead of serving the previous camera's ranges; ToupTek/PlayerOne/SVBONY pulse guides are serialized against overlapping calls (double-join UB / truncated pulses).
- **iOptron** (audit M20c + sweep): `set_park_position` sends properly zero-padded `:SPH`/`:SPA` commands; one transient serial timeout no longer permanently latches `device_faulted_` (3-consecutive-failure threshold, cleared on success); `RightAscensionRate` now returns the ASCOM offset-from-sidereal (0.0) instead of 15.041 arcsec/s in the wrong unit; all raw `stoll/stoi/stod` protocol parses wrapped to `AlpacaException` (ZWO mount wrapper swept identically).
- **WeeWX** (audit M20a/b): `supported_properties_` reads are now under the data mutex (raced the poll thread's inserts), and the stale-data clock is stamped from the payload's observation epoch instead of fetch time — a dead station's `TimeSinceLastUpdate` now actually grows.
- **Celestron/SynScan `is_aligned`** (audit low): ASCII `'0'` no longer counts as aligned (the `ch != 0` bug bypassed the slew-safety gate).
- **RGB24 channel order** (audit low): ZWO/PlayerOne/SVBONY BGR frames are reordered to R,G,B in the ImageArray (PlayerOne was already correct; verified).
- **Camera ROI pad-up** (audit low): ZWO and SVBONY replace align-down + fabricated black columns with ToupTek's pad-up + stride-crop, so clients get real pixels at the requested geometry (photometry-safe).
- **Base drivers & registry** (audit M4–M8): `RightAscensionRate` doc unit corrected (seconds of RA per sidereal second, not arcsec/s — a 15× trap); rotator `Move`/`MoveMechanical` doc comments un-swapped to match ASCOM; Dome and SafetyMonitor bases gain the Platform 7 `get_device_state()` override like their eight siblings; the device registry no longer invokes virtual driver getters while holding the global registry mutex (snapshot-then-call); `AsyncConnectable` no longer publishes `Connecting=false` before a deferred disconnect finishes tearing down.
- **HTTP: `persisted_devices_` raced across worker threads** (audit H7): every read/write of the persisted-device vector is now under a dedicated mutex (never held across registry/hardware calls).
- **QHY: `PulseGuide` blocked the HTTP thread for the full pulse duration** (ConformU on real miniCam8M hardware: a 2000ms pulse blocked the handler for exactly 2000ms, blowing the 1.0s STANDARD response-time target). `ControlQHYCCDGuide` now runs on the same detached-thread pattern as the Player One camera; the flag clears when the physical pulse completes.
- **QHY: `Connect()` could be silently dropped, leaving the camera stuck disconnected**: the router's disconnect-completion wait polled `get_connected()`, but the QHY driver (correctly, per AGENTS.md) clears that flag at the *start* of teardown rather than the end, so the router reported "disconnected" before the background task actually finished — the client's next `Connect()` then raced the still-running disconnect and was dropped by `AsyncConnectable`'s connect-vs-in-flight-disconnect rule. The wait now polls `get_connecting()` alone, the one signal guaranteed to hold for a task's full lifetime across all drivers sharing that base class.
- **QHY: disconnect could hang indefinitely on `SetQHYCCDParam`/`ControlQHYCCDTemp`**: both calls have no SDK-side timeout and occasionally ran far past their typical duration on real hardware, and the SDK wrapper's raw `qhyccd_handle*` meant a concurrent `close_camera()` could invalidate a handle a still-running call was using. The handle is now reference-counted (`shared_ptr` with a `CloseQHYCCD` deleter), so the physical close is deferred until every in-flight call actually finishes; the cooler-off and temp-control worker joins are bounded (2s, comfortably under ASCOM clients' ~5s `Disconnect()` budget) and detach on timeout instead of blocking the caller forever.
- **AsyncConnectable unit tests**: 4 new Catch2 test cases covering the shared connection-thread base directly (no hardware needed) — the get_connected()-is-not-a-completion-signal regression, and both directions of the connect-vs-in-flight-disconnect race rule.

### Changed
- **debian packaging** (audit): udev rules and firmware move to `/usr/lib/...` (Debian 13 usr-merge); `Conflicts/Replaces: fxload` declared for the bundled `fxload`; postinst `chown` no longer follows the packaged web symlink onto dpkg-owned `/usr/share` files; `debian/control` description fixed from `localhost:11111` to port 6800; AGPL headers added to the web UI assets (`app.js`, `style.css`, `index.html`); dead `vcpkg.json` removed.
- **Tests**: router-level path-traversal and `Content-Length`-bound regression tests; concurrency test for `persisted_devices_`; TSan `[stress]` coverage extended to the telescope drivers where the worst detached-thread bugs lived.

## [3.0.0] - 2026-07-06

### Added
- **ThreadSanitizer CI job + per-driver concurrency stress harness** (#101): a new `sanitizers-tsan` CI job builds the all-vendors AlpacaCore tests with `-fsanitize=thread` and runs a new `[stress]` suite — the first automated coverage for the #1 driver-review bug class (use-after-close, dropped racing disconnects, destructor vs connection-thread races), which the single-threaded ASan job and ConformU cannot see. The reusable harness (`AlpacaCore/tests/concurrency_stress.h`) provides three scenarios per driver: a lifecycle storm (async connect/disconnect + sync `set_connected` + status reads + operational calls from N threads), destruction racing an in-flight connect (the issue-#100 `std::terminate` class), and a deterministic racing-disconnect-never-dropped settle check. Registered for the drivers with the deepest race history — ToupTek camera, AFW filter wheel, and thermal switch run hardware-free over the fake SDK seam (issue #104), wrapped in a new thread-safe `LockedToupTekSDK` decorator so TSan findings point at driver code rather than the deliberately unhardened test fake; ZWO EFW and Player One Phoenix storm the failure path on hardware-free hosts (full path with a wheel attached). Wired into `scripts/ci_preflight.sh` behind `RUN_TSAN=1` (mirroring `RUN_SANITIZERS=1`) with a suppressions file that mutes only the uninstrumented proprietary vendor blobs. The CI job fails loudly if Catch2 is missing instead of green-lighting an empty suite.
- **Config save→load round-trip tests for every driver** (#102): `test_routing.cpp` now asserts field-by-field survival through `configuredevice` → `configureddevices` for all seventeen previously-uncovered (vendor, deviceType) configs — ZWO camera/filterwheel/focuser/rotator/switch (dew heater + ASIAIR libgpiod + ASIAIR Plus RK3568, including per-port `gpio`/`name`/`pwm` survival), QHY, SVBONY, ToupTek StellaVita (upgraded from no-crash to field survival), Player One camera (upgraded from configure-only) + filterwheel, Gemini focuser, WeeWX observing conditions, iOptron/SynScan/Bisque telescopes. A shared `roundtrip_config` helper keeps each block to its distinctive values and assertions.
- **Deterministic poll-until-settled decision logic** (#105): the "poll a device until it settles or times out" decision — where premature-settle, missed-timeout, and off-by-one-on-the-final-poll bugs live — is extracted into the pure `util::ConsecutiveSettle` state machine (`util/poll_settle.h`): consecutive-stable-reads requirement with bounce reset, poll budget, settle-beats-timeout on the final poll. The ToupTek AFW `wait_for_home` now feeds it (identical semantics: 3 stable reads, 60×100 ms budget), and eight scripted-sequence unit tests cover the batteries that previously needed hardware and real time — including the deceleration-bounce false-settle that shipped to review on PR #99. The camera exposure watchdog stays inline (a single deadline compare, not a state machine); the iOptron slew-settle loop shares the consecutive-run core but keeps its hand-rolled form by design (clock-deadline budget + dual exit — documented in AGENTS.md as the remaining instance).
- **Fault-injectable ToupTek SDK seam + hardware-free driver tests** (#104): the four ToupTek drivers (camera, focuser, filter wheel, thermal switch) now take the SDK through an abstract `ToupTekSDK` interface instead of hard-calling the `ToupTekSDKWrapper` singleton — production behavior is unchanged (default factories pass the singleton); new factory overloads accept an injected implementation. A scripted `FakeToupTekSDK` test backend (throw from any named call, canned device enumerations, reference-counted open/close accounting, scripted wheel-position sequences) makes the highest-risk driver paths testable without hardware. Five new test cases reproduce the PR #99 bug classes: connect-path failure must release the ref-counted open, `cameraIndex` resolves into the camera-only enumeration, camera + thermal switch share one physical open with last-holder-closes, AFW homing must not settle on a deceleration bounce, and a failed thermal-switch connect releases the shared open.
- **`RUN_SCAN_BUILD=1` pre-flight knob — Clang Static Analyzer, advisory** (#106): `scripts/ci_preflight.sh` can now run scan-build over the all-vendors build, with `unix.BlockInCriticalSection` disabled (the protocol wrappers intentionally hold the wrapper mutex across bounded serial/socket reads for transaction atomicity — 12 of the evaluation's 18 findings were that pattern). Advisory by decision: findings are reported with HTML output under `AlpacaHTTP/build-scan/scan-report/`, never fail the gate, and there is no CI job — the evaluation found zero results in the leak/use-after-free classes that motivated it (the analyzer does not see through the project's RAII + try/catch cleanup), so the CI time and triage burden aren't justified. Full triage on issue #106.
- **ToupTek AFW Filter Wheel driver** (AlpacaCore): standalone ToupTek AFW (Astro Filter Wheel) support for the AFW-M 5- and 7-slot models, enumerated through the existing toupcam Camera SDK (`Toupcam_EnumV2` filtered by `TOUPCAM_FLAG_FILTERWHEEL`). At connect the driver reads the slot count (`TOUPCAM_OPTION_FILTERWHEEL_SLOT`), writes it back, and homes the wheel (`TOUPCAM_OPTION_FILTERWHEEL_POSITION = -1`) — the same sequence as the INDI toupbase reference driver — so the firmware establishes its slot reference; without homing the wheel hunts and never lands (notably right after a firmware update). `Position` reads/writes then go through `TOUPCAM_OPTION_FILTERWHEEL_POSITION` as single absolute moves (clockwise direction bit), where the SDK's in-motion `-1` maps directly onto the ASCOM "moving" sentinel. `Names`/`FocusOffsets` follow the shared ZWO EFW / Player One Phoenix semantics (length tied to slot count, single-string expansion, default `Filter N` names). Interface version 3 (IFilterWheelV3). The toupcam SDK version is surfaced in the web UI only, never in `DriverInfo`.
- **ToupTek AFW support** (AlpacaHTTP): router registration (`touptek` + `filterwheel`, by index or SDK id with optional `filterNames`), and web UI — ToupTek now offers FilterWheel as a device type with a 5/7/Custom slot lineup (AFW-M models) reusing the shared `createFilterwheelSlotUI`, plus `touptekFilterwheelIndex` per-`(vendor, deviceType)` index auto-numbering.
- **ToupTek AFW unit tests** (AlpacaCore): 8 Catch2 test cases, 43 assertions.
- **ToupTek readout modes: conversion gain + High Full Well** (AlpacaCore): cooled ToupTek cameras now fold their conversion-gain (`TOUPCAM_OPTION_CG` — HCG/LCG, plus HDR on `FLAG_CGHDR`) and High Full Well (`TOUPCAM_OPTION_HIGH_FULLWELL`) hardware modes into one flat ASCOM `ReadoutModes` list (e.g. `HCG / LCG / High Full Well` on the ATR2600M / IMX571). Each mode fully specifies the state it applies on both axes, so a read round-trips to a stable index. NINA renders it as a Readout Mode dropdown — the idiomatic home for these sensor modes (no custom Action). Cameras with neither capability keep the single "Normal" mode and behave exactly as before.
- **ToupTek camera Offset** (AlpacaCore): cameras reporting `TOUPCAM_FLAG_BLACKLEVEL` now expose ASCOM `Offset` (integer `OffsetMin`/`OffsetMax` mode) mapped to `TOUPCAM_OPTION_BLACKLEVEL`, matching the ZWO/SVBONY/Player One/QHY camera drivers — previously ToupTek was the only camera driver that threw `PropertyNotImplemented` for offset. `OffsetMax` scales with the current output bit depth (31 at 8-bit up to 31×256 at 16-bit). Cameras without the flag still report `PropertyNotImplemented`.
- **ToupTek Thermal Switch driver** (AlpacaCore): a cooled ToupTek camera's anti-fog dew heater (`TOUPCAM_OPTION_HEAT`, level 0..`HEAT_MAX`), radiator fan (`TOUPCAM_OPTION_FAN`, speed 0..`model->maxfanspeed`), and tail indicator LED (`TOUPCAM_OPTION_TAILLIGHT`, on/off — astro users turn it off to avoid light leaks) as ASCOM switch elements, capability-probed per model at connect (`TOUPCAM_FLAG_HEAT` / `_FAN`; the tail LED has no flag so it is probed by reading the option) — a camera exposing none of them fails to connect with `NotImplemented`. Mirrors the Player One thermal switch; binds by `cameraIndex` and shares the camera's Toupcam handle via a new reference-counted open in `ToupTekSDKWrapper` (`Toupcam_Open` fires once, `Toupcam_Close` on the last release), so it runs alongside the camera device on one physical open. The cooler itself stays on the Camera interface (`CoolerOn` / `SetCCDTemperature`), never a switch element.
- **ToupTek Thermal Switch support** (AlpacaHTTP): the `(touptek, switch)` route now selects between two backends via `switchType` — `thermal` (camera dew heater + fan, bound by `cameraIndex`) and `stellavita` (the existing GPIO PowerBox, the default). Web UI adds a Switch Type selector with the thermal camera-index field.
- **ToupTek cooled-camera unit tests** (AlpacaCore): thermal switch (8 Catch2 test cases, 43 assertions) plus a High Full Well readout-mode case on the camera suite.
- **Device firmware & SDK version in the web UI** (#93): two new optional driver hooks — `AlpacaDriver::get_device_firmware()` (the device's own hardware firmware) and `AlpacaDriver::get_device_sdk_version()` (the vendor library version) — kept distinct so a vendor SDK version is never mislabeled as device firmware. Both are surfaced **only** in the AlpacaBridge web UI: the management `configureddevices` response gains per-device `Firmware` and/or `SdkVersion` fields when the connected driver reports them, and the device-details panel renders a "Firmware" and/or "SDK Version" row only when present. Neither is **ever** added to the ASCOM `DriverInfo` string, so NINA and other Alpaca clients keep a clean driver string; `DriverVersion` remains the AlpacaBridge software version everywhere. Real firmware: WandererCover (date from the streamed status frame), the Gemini focuser and SynScan / Celestron handsets (captured once at connect — no per-request serial I/O), and SVBONY cameras (`SVBGetCameraFirmwareVersion`). SDK version: ZWO camera / EFW filter wheel / EAF focuser / CAA rotator (ASI/EFW/EAF/CAA SDK, normalized from `1, 7, 7, 0` to `1.7.7.0`), and QHY / SVBONY / Player One cameras — the ASI/QHY/Player One SDKs expose no device-firmware API (SVBONY reports both its SDK version and real device firmware).

### Changed
- **License: SSPL v1 → AGPL-3.0-or-later with a vendor-SDK linking exception** (#113): AlpacaBridge is now licensed under the GNU Affero General Public License, version 3 or later, replacing the Server Side Public License v1 across the repository (`LICENSE` files, all 210 source-file headers, packaging metadata, and contributor docs). SSPL is not OSI-approved and is classified non-free by Debian/Fedora, which blocked AlpacaBridge from distro and third-party apt repos; AGPL §13 still covers the actual concern (a closed-hardware vendor embedding the bridge and serving its Alpaca API over the network triggers the source obligation). Because the bridge links proprietary vendor device SDKs, the copyright holders grant a GPLv3 §7 additional permission (appended to `LICENSE`) allowing AlpacaBridge to be combined with those SDKs and conveyed without the SDKs becoming AGPL-licensed. `debian/copyright` now embeds the full AGPL text (Debian ships no `common-licenses/AGPL-3`) and lists every tracked `external/` component accurately (ZWO is MIT, Player One permissive-with-notice, QHY/ToupTek/SVBONY proprietary-unmodified, libgpiod LGPL-2.1-or-later, WandererAstro vendor docs), and `AlpacaCore/external/README.md` documents each vendored SDK's provenance and license status. The change applies prospectively — revisions already published under SSPL remain SSPL at those historical revisions.

### Fixed
- **ZWO camera: exposure-status read guarded against a racing disconnect** (#120): `poll_exposure_status` read the SDK status on a bare id snapshot without a guard, so a disconnect closing the camera in that window could surface a raw SDK exception through `CameraState`/`ImageReady` instead of a well-formed Alpaca response; it now returns Idle on a throwing read, matching the guards on the retry burst. Also aligns the `debian/copyright` ZWO copyright-holder name with the SDK's `license.txt` ("ZWO Company") in the mount-docs stanza (PR #118 approval note).
- **ZWO / Player One / SVBONY cameras: operational calls could race a disconnect onto a just-closed camera** (#116): the three camera drivers snapshotted the camera id under `mutex_` and then called the SDK after releasing the lock (`camera_id_value()` / `camera_id_copy()`), so a concurrent disconnect could close the camera between the snapshot and the SDK call — the same class as the PR #115 round-3 switch finding, deferred then because camera disconnects are entangled with exposure-worker lifecycles. All fast register/control calls now run under a `with_camera` helper that holds `mutex_` across the SDK call (the ToupTek `with_handle` shape); the exposure workers and SVBONY's duration-blocking `SVBPulseGuide` keep their bare snapshots by design (they must not hold the lock across blocking waits) and are documented as such. Each disconnect path now clears driver state and publishes `connected_ = false` **before** the SDK stop/close (matching the switch/EFW siblings and the AGENTS.md rule), which also means a throwing close can no longer trap the driver half-connected. Bonus fix from the same audit: `get_control_caps_or_throw` returned a reference into the caps map after releasing the lock — a dangling read once a reconnect reloaded the map — and now returns by value in the two drivers that have it (ZWO, SVBONY; Player One caches caps differently). Six new `[stress]` cases (ZWO camera, Player One camera, and a new SVBONY stress file) storm the converted gates and the destructor-vs-connect race under the TSan CI job.
- **ZWO camera: one transient exposure failure poisoned every subsequent operation (18 ConformU issues); QHY had the same sticky state**: ConformU 4.4.0 on an ASI2600MM Pro hit a one-off `ASI_EXP_FAILED` on the 4×4-bin exposure (unreproducible in four manual retries — the known transient SDK/USB class), and the driver mapped it to a `CameraState::Error` that nothing ever cleared, so all 17 remaining exposure tests were abandoned with "camera is in state: Error rather than the expected: Idle". The ZWO driver now (1) auto-retries a failed in-flight exposure up to 2 times (the same policy as INDI's ASI driver) before latching the failure, and (2) reports `Idle` once latched — a failed exposure leaves the camera fully ready for the next one; the failure still surfaces through `ImageReady` staying false and `ImageArray` throwing `Exposure failed`. The sibling sweep found the QHY camera mapping its driver-side `Failed` latch to the same sticky `Error` (fixed identically); Player One and SVBONY already resolve to Idle via their worker state machines and deadline watchdogs.
- **Local `RUN_TSAN=1` pre-flight could false-PASS with zero stress tests under Catch2 v2** (#117): the zero-test guard counted `--list-tests --verbosity quiet` output lines, which is Catch2 v3 syntax — under v2 a zero-match filter still prints one line, so the `wc -l ≥ 1` check always passed. Both the pre-flight and the `sanitizers-tsan` CI job now assert on the Catch2 run summary (`test cases: N` with N ≥ 1), which reflects what actually executed and is version-agnostic.
- **`.deb` left the ASIAIR Plus (RK3568) switch EACCES-dead on a stock-kernel box**: two packaging gaps, found and fixed during the issue-#107 hardware re-validation sweep. (1) `debian/rules` hand-lists udev rules and never shipped `AlpacaCore/external/ZWO/asiair-plus/99-zwo-asiair-plus.rules` — the rule that opens ZWO's root-only `/dev/pwm-gpio-misc` to the `gpio` group — even though both shell installers pick it up via their `*.rules` discovery loop. (2) The postinst only enrolled the service user in the `gpio` group when the group already existed; ZWO's stock RK3568 kernel image ships none, so the rule's `GROUP="gpio"` silently fell back to root. The postinst now creates the system group (`groupadd -f --system gpio`, mirroring `build_and_run.sh`) and always enrolls the service user; the rule ships in the package. Validated end-to-end on stock-kernel RK3568 hardware (ConformU 4.4.0 clean).
- **Web UI: ToupTek Switch Type selector lists Camera Thermal first**: the thermal (camera dew heater/fan) backend is now the first option and the default for newly configured ToupTek switches; StellaVita moved second. Saved configs are unaffected — the load path still defaults legacy configs (no `switchType`) to StellaVita, and the router's backend default is unchanged.
- **Every vendor driver: a `connect()` racing the destructor could spawn a connection thread that outlives the object → `std::terminate`; a disconnect racing an in-flight connect could be silently dropped** (#100): the async connection-thread lifecycle — previously copy-pasted across ~26 drivers, with only the four ToupTek drivers carrying the PR #99 `shutting_down_` destructor guard and never-drop-a-racing-disconnect protocol — is extracted into one shared `AsyncConnectable` mixin (`AlpacaCore/include/alpacacore/async_connectable.h`) and **all 26 vendor drivers now inherit it**: ZWO (camera/telescope/focuser/filterwheel/rotator/switch/ASIAIR/ASIAIR Plus), ToupTek (camera/focuser/filterwheel/thermal switch/StellaVita), Player One (camera/filterwheel/switch), QHY, SVBONY, iOptron (telescope/switch), SynScan, Celestron, Bisque, Gemini, WandererAstro, WeeWX. The base owns the full battle-tested protocol from the PR #99 review marathon (spawn guard, pending-disconnect record/consume, Idle-published-under-the-lock task tail, join-outside-the-lock) plus a rollback for a throwing `std::thread` constructor (previously only WandererAstro guarded this — everywhere else it wedged the driver unconnectable forever). Drivers that additionally lacked the racing-disconnect protocol (all 22 non-ToupTek) gain it for free. The AFW keeps its homing-window `connecting_homing_` guard on top via the base's `record_pending_disconnect()` hook. AGENTS.md now points new drivers at the base instead of the checklist prose. libqhyccd's first entry point spawns its `PnpEventListenerThread`, which calls `libusb_hotplug_register_callback` on the failed libusb context and segfaults the whole process — so merely registering a QHY camera (construction-time model preload) or answering a `configureddevices` poll (SDK-version query) killed AlpacaBridge on any USB-less machine. Found by the new round-trip tests on the CI runner and reproduced in a USB-hidden sandbox with a full backtrace. The wrapper now makes no libqhyccd call until the first real camera operation (lazy `ensure_resource` at connect), the SDK version is served from a cache filled at init (empty before the first connect), and the construction-time model preload is removed — the web UI shows the model after the first Connect instead of immediately. Connect on a genuinely broken-USB host still trips the SDK bug, but configuration and management never do.
- **iOptron and SynScan telescope `mountIndex` silently dropped on save** (#102): both registration paths read `mountIndex` (auto-detect device selection) but neither sanitizer branch allowlisted it, so a saved non-zero mount index reverted to 0 on the next load — the exact silent-data-loss class the new round-trip tests exist to catch (found by the issue-#102 field inventory; Celestron was the only mount vendor with the key allowlisted). Both branches now copy it, and the new telescope round-trip tests assert it.
- **ToupTek camera: HFW disabled before CG when leaving High Full Well** (AlpacaCore): `set_readout_mode` wrote `OPTION_CG` before `OPTION_HIGH_FULLWELL` unconditionally, so an HFW → HCG/LCG transition set CG while HFW was still enabled — if the firmware treats the two as mutually exclusive it would reject the write and leave the camera stuck in HFW with only half the mode applied. The writes are now ordered by direction: HFW is disabled *before* CG when the target has HFW off, and enabled *after* CG when the target has it on. (Entering HFW keeps the CG-first order; ConformU can't exercise a live HFW→HCG sequence, so this is defensive against firmware behavior.)
- **ToupTek thermal switch: `camera_name_` reset on failed connect and disconnect** (AlpacaCore): the name was assigned before the open but never reset, unlike every other identity field — harmless (its only consumer is connection-gated) but now cleared to the constructor default on both cleanup paths for full symmetry.
- **ToupTek camera: Offset accessors' support check folded under the operation's lock** (AlpacaCore): `get_offset`/`set_offset`/`get_offset_max`/`get_offset_min` called `ensure_blacklevel_supported()` (which took and released `mutex_`) and then re-acquired the lock for the body — a concurrent reconnect to a model without `TOUPCAM_FLAG_BLACKLEVEL` in that gap made the body call the SDK on an unsupporting handle, surfacing a `DriverException` instead of `PropertyNotImplemented`. The check is now `ensure_blacklevel_supported_locked()`, run inside the same `mutex_` hold as the SDK call in all four accessors.
- **ToupTek camera: `ReadoutModes` list readable while disconnected again** (AlpacaCore): an earlier round added `ensure_connected()` to `get_readout_modes`, but every other camera driver (ZWO, QHY, SVBONY, Player One) returns a static/cached list while disconnected, and imaging clients (NINA/APT/SGPro) enumerate `ReadoutModes` before connecting to populate dropdowns. Reverted for cross-driver consistency: the list derives from the preloaded model caps (or `Normal`) while disconnected; the current-index getter/setter still require a live connection. Unit test updated.
- **ToupTek thermal switch: stale SN-based `UniqueID` after disconnect** (AlpacaCore): the disconnect path cleared `camera_id_`/`elements_` but not `serial_number_`, so `get_unique_id()` kept reporting the previous camera's SN-based ID while disconnected instead of the device-number fallback. Now cleared, matching the camera driver's disconnect path (the failed-connect catch already did).
- **ToupTek devices: the connection tail's own deferred disconnect could re-trigger the recording gate (AFW; hardened everywhere)** (AlpacaCore): the tail ran its deferred `set_connected(false)` while `conn_task_` was still `ConnectInFlight`, so the AFW's sync gate treated the driver's own call as another racing disconnect — it re-recorded the flag and returned without closing, leaving the wheel **Connected** after a connect-then-rapid-disconnect *and* the stale flag silently no-opping the next connect. The tail now publishes `Idle` **before** the deferred disconnect (still under `connection_mutex_`, so recorders cannot misjudge the state) in all three drivers, and the AFW gate gains the `!connected_` conjunct the camera/thermal gates already had.
- **Web UI: thermal-switch camera-index fields missing from auto-numbering (ToupTek + Player One)** (AlpacaHTTP): `INDEX_FIELDS` lacked entries for `touptek-thermal-camera-index` and `playerone-switch-camera-index`, so on a two-camera rig the second thermal switch's camera index stayed at 0 — silently binding it to the first camera unless hand-corrected. Both entries added, matching every other per-vendor camera-index field.
- **ToupTek devices: connection-task tail now publishes Idle under `connection_mutex_` — no disconnect can be dropped (AFW, thermal switch, camera)** (AlpacaCore): `start_connection_task` read `conn_task_` twice while the connection thread stored `Idle` without the lock, so between the two reads the task could finish and a disconnect's intent-recording was skipped (silently dropped, device stays connected). The task's tail (pending-disconnect re-check + `Idle` store) now runs under `connection_mutex_` — the same lock every `start_connection_task` holds — so `conn_task_` transitions are lock-protected end to end: a racing disconnect either records its intent against a still-in-flight connect (honored by the tail, which now runs the deferred disconnect itself) or observes `Idle` and spawns a real disconnect task. This *eliminates* the previously documented instructions-wide dropped-disconnect window rather than shrinking it, and the recorder now uses a single snapshot read. `stop_connection_thread` joins outside `connection_mutex_` (the tail takes it, so joining under it would deadlock).
- **Cameras: `set_num_x`/`set_num_y` (and StartX/StartY) could silently clobber the other axis (all five camera drivers)** (AlpacaCore): each setter snapshotted the other axis via a separate locked getter and then wrote **both** fields under a later lock (`set_roi_size_locked(num_x, get_num_y())`), so two concurrent axis setters could revert each other with a stale snapshot (lost-update). The shared helpers now take `std::optional<int>` per axis — `nullopt` means "leave unchanged", resolved *under* `mutex_` — so each public setter passes only its own axis and a client-supplied `-1` is still rejected as InvalidValue. Template pattern fixed in ToupTek, ZWO, SVBONY, Player One, and QHY at once.
- **ToupTek devices: connection-task state collapsed into one atomic (AFW, thermal switch, camera)** (AlpacaCore): the in-flight flag (`connecting_`) and its direction (`inflight_is_connect_`) were two separate atomics, leaving an unavoidable single-instruction window between the stores in which the synchronous `Connected=false` gate could observe a half-published pair, misjudge the in-flight task, and drop the disconnect — whichever store order was chosen, one flavor of the window remained. Both are now one `conn_task_` atomic (`Idle` / `ConnectInFlight` / `DisconnectInFlight`) published in a single store, eliminating the class rather than shrinking the window. `get_connecting()` reads `!= Idle`; net removal of one atomic per driver.
- **ToupTek devices: a leaked `pending_disconnect_` made the next connect silently no-op (AFW, thermal switch, camera)** (AlpacaCore): a disconnect landing in the instructions-wide window between the connection task's final flag re-check and `connecting_ = false` left the flag set; that disconnect is unavoidably dropped, but the stale flag was then consumed by the **next** connect's entry check, which returned without connecting — no error, no log, `Connecting`/`Connected` both false — forcing the client to connect twice. All three connection tasks now clear the flag (under `mutex_`) immediately before `connecting_ = false`, so a raced-out disconnect costs a retry of the *disconnect* but never poisons a future connect.
- **ToupTek thermal switch + camera: async disconnect racing an in-flight connect was silently dropped** (AlpacaCore): both drivers' `start_connection_task(false)` returned at the `connecting_` guard without recording anything, so a `Disconnect` arriving before the connect thread ran (or while it held `mutex_`) was discarded and the device came up Connected despite the explicit request — the same defect class just fixed in the AFW filter wheel. Both now carry the same machinery: `inflight_is_connect_` tracks the in-flight task's direction, both disconnect routes record `pending_disconnect_` whenever a connect is in flight, `set_connected(true)` consumes the flag at entry (a newer disconnect supersedes — stays disconnected), and the connection task re-checks after finishing (disconnecting if the connect already published). All three ToupTek devices in this PR now handle the race identically; the remaining pre-existing drivers are tracked for the shared AsyncConnectable rework in #100.
- **ToupTek camera: unrecognised CG/HFW register combination now logged** (AlpacaCore): `get_readout_mode` silently reported mode 0 when the live registers matched no enumerated spec (only reachable if the camera powers up in an undocumented mode); a warning now makes a real occurrence observable.
- **ToupTek AFW: async disconnect could still be dropped in the connect startup window** (AlpacaCore): `pending_disconnect_` was only recorded while `connecting_homing_` was true, but that flag is set by the connect *thread* after it acquires `mutex_` — so a disconnect landing between `connecting_ = true` in the spawner and the thread taking the lock (a window widened in practice by property getters contending on `mutex_`) saw `connecting_homing_ == false`, recorded nothing, and was silently dropped; the wheel came up Connected despite the explicit request. A new `inflight_is_connect_` atomic tracks the in-flight task's direction: both disconnect routes now record `pending_disconnect_` whenever a *connect* is in flight; `set_connected(true)` consumes the flag at entry (a newer disconnect supersedes the connect — stays disconnected), the post-homing check consumes it mid-poll, and the connection task re-checks after finishing (running the disconnect if the connect had already published). The only remaining exposure is a few instructions before `connecting_ = false`, which self-heals on the next connect.
- **Cameras: exposure-thread join could race a concurrent spawn (ToupTek, SVBONY, Player One, QHY)** (AlpacaCore): `stop_exposure`/`AbortExposure` joins `exposure_thread_` while `start_exposure` assigns it — `join()` racing `operator=` on the same `std::thread` is undefined behaviour, and on the disconnect path a `start_exposure` slipping between the exposure-thread join and the SDK close could hand a freshly-spawned thread a camera that is closed under it (for ToupTek a raw-pointer use-after-close inside `WaitImageV4`). A new per-driver `exposure_lifecycle_mutex_` (ordered before `mutex_`; the exposure thread itself never takes it) now serialises the thread's lifecycle: `start_exposure` holds it through the spawn, `stop_exposure` through the join, and `set_connected(false)` from the join through `close` — so a spawn can never land in the join→close gap, and joins never race assignments. ToupTek's async connection task drops its now-redundant (and unguarded) pre-stop; destructors remain exempt (they run after the connection thread is joined, with no client calls in flight). ZWO has no exposure thread.
- **ToupTek AFW: a failed connect could permanently lock out reconnects** (AlpacaCore): `connecting_homing_` was set before `resolve_wheel_locked()` and `open_filter_wheel_by_id()`, but both sit before the `try` whose catch clears the flag — so a connect that failed at enumeration/open (wheel unplugged, id not found) left the flag stuck true, and the double-open guard then silently no-opped **every** subsequent connect until the driver was destroyed. Both calls now run inside the try (the catch skips the SDK close when the open itself threw), so the flag is always cleared on failure.
- **Cameras: sync `set_connected(false)` closed the SDK session under a live exposure thread (ToupTek, SVBONY, Player One)** (AlpacaCore): the async disconnect path stops/joins the exposure thread before closing, but the public synchronous `set_connected(false)` did not — for ToupTek that is a use-after-close (the thread runs `Toupcam_WaitImageV4` on a raw handle snapshot); for SVBONY/Player One the async connection task calls `set_connected` directly, so **both** routes closed the camera with the exposure loop possibly live. `set_connected(false)` now stops and joins the exposure thread before taking `mutex_` (outside the lock — the thread takes `mutex_` to publish results, and ToupTek's stop takes it internally, so joining under the lock would self-deadlock); redundant with the async path's earlier stop, which remains a harmless no-op. QHY checked and unchanged: its disconnect already cancels the exposure before closing and its wrapper's string-id indirection fails closed; ZWO has no exposure thread.
- **Filter wheels: `SetPosition(-1)` returned NotConnected instead of InvalidValue while disconnected (ZWO EFW, Player One Phoenix)** (AlpacaCore): both had the negative-position check after `ensure_connected()`, violating the ASCOM precedence rule (range validation precedes the connection check) that the ToupTek AFW driver already implements. The negative check now runs first in both; the upper bound still reports NotConnected while disconnected (slot count unknown until connect). Unit tests extended to assert the disconnected `-1 → InvalidValue` path in both drivers.
- **Cameras: a redundant Connect wiped exposure state (ToupTek, ZWO, SVBONY, Player One)** (AlpacaCore): the idempotent-connect branch (`connected == connected_`) called `reset_exposure_state_locked()` when already connected, clearing `exposure_active_`/`image_ready_`/`last_exposure_valid_`. Because the Platform-7 `connect` endpoint calls `connect()` **unconditionally** (no `!get_connected()` guard, unlike the legacy `Connected=true` PUT), a client hitting it during an exposure would flip `CameraState` to Idle mid-frame (letting a concurrent `set_gain`/`set_readout_mode` write registers into the live integration) or discard a just-completed image. ASCOM requires Connect to be idempotent, so the redundant branch now returns without touching exposure state — matching the QHY driver, which already did this correctly. Found by proactive cross-driver sweep; fixed in all four affected camera drivers at once.
- **ToupTek camera: `IsPulseGuiding` swallowed SDK errors as "not guiding"** (AlpacaCore): `is_guiding` judged `Toupcam_ST4PlusGuideState` with `hr == 0`, so `S_FALSE` (0x1 — a distinct SUCCESS code the SDK header warns against conflating) *and* every negative error HRESULT both fell through to `return false`. A transient SDK error would then clear the driver's in-flight guide flag while the ST4 hardware was still pulsing, letting a guider polling `IsPulseGuiding` fire the next move mid-pulse. It now calls `throw_on_error` (surfacing real errors) and returns `hr == S_OK`, so only a genuine "not guiding" success maps to false.
- **ToupTek AFW: an async `disconnect()` during homing was also silently dropped** (AlpacaCore): the previous fix covered the sync `set_connected(false)` path, but the router dispatches both the legacy `Connected=false` PUT and the Alpaca v2 `disconnect` endpoint through `disconnect()` → `start_connection_task(false)`, which returned at its `connecting_` guard without recording the request — so a `disconnect` arriving during the ~1.5–6 s homing poll still left the wheel `Connected`. `start_connection_task(false)` now sets the same `pending_disconnect_` flag (under `mutex_`, gated on `connecting_homing_`) when a connect is mid-homing, so the in-flight connect aborts after homing on either disconnect route. (Fixes the shared pattern the sync-only fix missed.)
- **Web UI: reflected XSS via unescaped `ErrorMessage` in two error paths** (AlpacaHTTP): `loadDevices()` and `loadServerInfo()` dropped a server-supplied `data.ErrorMessage` straight into `innerHTML` (`app.js` lines 425/1032), so an error string containing markup (e.g. `<img src=x onerror=…>`) would execute in the admin UI. Both now wrap it in the existing `escapeHtml()` helper, matching every other dynamic-content site in the file (the remaining `ErrorMessage` uses go through `textContent`, `alert()`, or `new Error()` and are not injection points).
- **ToupTek AFW: a synchronous `Connected = false` during homing was silently dropped** (AlpacaCore): `set_connected(true)` releases `mutex_` for the ~1.5–6 s homing poll; a sync `set_connected(false)` (the ASCOM `Connected` setter — bypasses the async `connecting_` guard) landing in that window hit the `connected == connected_` early-return (both still false) and did nothing, so the wheel came up **Connected despite an explicit disconnect**. A `pending_disconnect_` flag (guarded by `mutex_`) now records the request; the in-flight connect re-checks it after homing and cleanly aborts — closes the freshly-opened handle and stays disconnected — instead of publishing. The flag is cleared at the start of each connect and consumed on the failure path. (The async disconnect path is already blocked by `connecting_`; the broader disconnect-during-connect cancellation is tracked in #100.)
- **ToupTek camera: `get_readout_mode` matched a pre-lock spec snapshot against under-lock register reads** (AlpacaCore): the getter captured the mode list via `readout_mode_specs()` (a `camera_info_` snapshot under a since-released `mutex_`), then read the live `CG`/`HFW` registers under `mutex_` + `readout_mutex_` — so a disconnect+reconnect to a different model in that window matched the new camera's registers against the old model's mode list, potentially returning the wrong index (e.g. "LCG" for a non-CG camera). It now derives the spec list via `readout_mode_specs_locked()` inside the same double-lock as the register reads, so the list, capabilities, and registers all come from one connection — the exact symmetry the `set_readout_mode` fix relies on. (Completes the previous entry: both directions now derive the spec under the lock they act on.)
- **ToupTek camera: `set_readout_mode` derived the mode spec outside the lock it applied under** (AlpacaCore): the setter called `readout_mode_specs()` (a `camera_info_` snapshot) for its range check and then applied the chosen spec's CG/HFW writes under `mutex_` + `readout_mutex_` — so a disconnect+reconnect to a *different* model in the gap could pair an old model's spec (`s.cg`/`s.set_hfw`) with the new handle, the write asymmetry the getter (`get_readout_mode`) had already avoided by re-deriving capabilities under both locks. `readout_mode_specs()` is now split into a public wrapper and a `readout_mode_specs_locked()` that assumes `mutex_` is held; `set_readout_mode` keeps the pre-lock range check (InvalidValue-before-NotConnected precedence while disconnected) but re-derives the spec from `readout_mode_specs_locked()` under `mutex_` + `readout_mutex_`, re-validates the index against the live model, and applies from that one snapshot. The Normal-only no-write case now falls out naturally (neither axis set) while still running the mid-exposure guard.
- **ToupTek camera: `start_exposure` read the exposure range with a snapshot handle** (AlpacaCore): the `with_handle()` sweep converted every fast getter/setter, but `start_exposure` still validated the requested duration by calling `get_exposure_range(handle_copy())` — the one remaining `handle_copy()` read-then-use, so a concurrent `set_connected(false)` could `Toupcam_Close` the handle between the snapshot and `Toupcam_get_ExpTimeRange` (use-after-close; reachable when the camera runs without the thermal switch, ref count 1). The range read now runs under `with_handle()` (`mutex_` held across the SDK call). The separate `handle` snapshot captured for the exposure thread is unchanged — that use is safe because `stop_exposure_thread()` joins the thread before any close.
- **ToupTek camera: closed the use-after-close window across every SDK getter/setter** (AlpacaCore): the `handle_copy()` idiom snapshotted `handle_` under `mutex_` and then released the lock *before* the SDK call, so a concurrent `set_connected(false)` (which closes the handle under `mutex_`) could `Toupcam_Close` the handle mid-call — a latent use-after-close in `get_ccd_temperature`, cooler on/off/power, exposure range, gain get/min/max, `IsPulseGuiding`, `Offset` get, cooler set-point get/set, `PulseGuide`, and `OffsetMax`. A new `with_handle()` helper runs each fast SDK option read/write with `mutex_` held across the call; `set_readout_mode` (previously still on the snapshot idiom — it used a stale handle if a disconnect+reconnect completed after `handle_copy()`) now holds `mutex_` + `readout_mutex_` and reads `handle_` directly, matching `set_gain`/`set_offset`. The exposure thread's own handle snapshot is unchanged (it must not hold `mutex_` across `WaitImageV4`; its close is already stop-and-joined first).
- **ToupTek Thermal Switch: partial `elements_` left populated on a failed connect** (AlpacaCore): if `build_elements_locked` threw after pushing some elements, the connect catch cleared `camera_id_`/`serial_number_` but not `elements_`. It now clears `elements_` too, so the disconnected state is fully clean (was unreachable in practice — accessors guard on the connection — but consistent with the rest of the cleanup).
- **ToupTek AFW: racing synchronous connect could double-open and leak the SDK handle** (AlpacaCore): the filter wheel is the only driver that releases `mutex_` during connect (for the ~1.5–6 s homing poll), and `set_connected` is a public sync entry point that bypasses `start_connection_task`'s `connecting_` guard — so a second `set_connected(true)` landing in the homing window saw `connected_ == false`, opened the same wheel again (ref count → 2), and the handle then leaked for the process lifetime when only one close fired. A `connecting_homing_` flag (guarded by `mutex_`) now short-circuits a racing connect so only one open happens.
- **ToupTek camera: `set_gain`/`set_offset` range bound was read outside `readout_mutex_`** (AlpacaCore): both read the valid range from the SDK (gain range; black-level max, which scales with the live bit depth) and range-checked *before* taking `readout_mutex_`, so a concurrent reconnect to a different model/bit depth could stale the accepted bound between the check and the register write (a spurious SDK `DriverException`). Both now read the range, range-check, and write under `mutex_` + `readout_mutex_` held together — one consistent connection snapshot, reading `handle_` directly rather than a pre-lock copy.
- **ToupTek camera: `get_readout_mode` could read SDK registers with a stale handle/capability pair** (AlpacaCore): the handle and the CG/HFW capability flags were snapshotted in two separate `mutex_` acquisitions before taking `readout_mutex_`, so a disconnect (or disconnect+reconnect to a different model) between them could leave the pre-lock `has_cg`/`has_hfw` inconsistent with the handle actually used. It now reads the handle, derives the capability flags from `camera_info_`, and reads both SDK registers under `mutex_` + `readout_mutex_` held together — one consistent connection snapshot, still atomic against `set_readout_mode`'s two-step apply.
- **ToupTek SDK wrapper: `open_camera_by_index` marked `[[deprecated]]`** (AlpacaCore): its index is the SDK's raw enumeration order (includes AFW/AAF) rather than the camera-only index space, and it opens outside the ref-counted by-id sharing, so it could close a device another driver holds open by id. No production path uses it; the attribute enforces that any new caller is warned toward `open_camera_by_id`.
- **ToupTek camera: user-initiated disconnect could close the handle under an in-flight exposure** (AlpacaCore): the connection-task `set_connected(false)` path stopped and closed the SDK handle without first joining the exposure thread, which runs `Toupcam_WaitImageV4` holding none of the driver locks — so if `Stop` unwound the wait asynchronously, `Toupcam_Close` could race a live `WaitImageV4` (use-after-close). The destructor already stopped+joined the exposure thread before closing; the connection task now does the same (`stop_exposure_thread()` before dispatching the disconnect), which it must do outside `set_connected` since that already holds `mutex_` (the join would otherwise self-deadlock).
- **ToupTek AFW: `SetPosition(-1)` returned NotConnected instead of InvalidValue while disconnected** (AlpacaCore): `set_position` ran `ensure_connected()` before the range check, so a negative position while disconnected reported `NotConnected`. Per the ASCOM precedence rule (range validation before the connection check), a negative position is now rejected as `InvalidValue` first; the upper bound still reports `NotConnected` when disconnected (it depends on the slot count, unknown until connect). Unit test updated accordingly.
- **ToupTek camera: null-model device could enumerate as a phantom camera at index 0** (AlpacaCore): `enumerate_cameras` guarded the AFW/AAF skip with `arr[i].model && …`, so an entry whose `model` block was null short-circuited to *not skipped* and was pushed as a camera with zero flags/dimensions at index 0 — displacing the real camera. `enumerate_focusers` and `enumerate_filter_wheels` already had an explicit `if (!arr[i].model) continue;`; `enumerate_cameras` now matches. Per the "fix shared patterns everywhere" rule.
- **ToupTek camera: `ImageReady` could read true while `CameraState` still reported Exposing** (AlpacaCore): the exposure thread published `image_ready_ = true` (under `mutex_`) before clearing `exposure_active_` (under `readout_mutex_`), so a client polling in that window saw the ASCOM-forbidden `Exposing` + `ImageReady=true` combination (the prior round's `readout_mutex_` addition widened it). The thread now builds/caches the frame while still Exposing, clears `exposure_active_` first, and only then publishes `image_ready_` — so `ImageReady` never turns true until the camera has left the Exposing state.
- **ToupTek camera: redundant reconnect cleared `exposure_active_` off `readout_mutex_`** (AlpacaCore): a `set_connected(true)` on an already-connected camera reset exposure state under `mutex_` alone, bypassing the invariant that `exposure_active_` only changes under `readout_mutex_` (a concurrent `set_gain`/`set_readout_mode` could then pass `ensure_not_exposing` and write registers mid-frame). Both the redundant-reconnect and post-connect reset paths now hold `readout_mutex_`, so every `exposure_active_` transition is covered.
- **ToupTek Thermal Switch: `set_switch(bool)` had a cross-call TOCTOU** (AlpacaCore): it resolved the target level via `get_max/min_switch_value` and then wrote it via `set_switch_value` — three separate lock acquisitions — so a disconnect+reconnect to a different camera model in between could change the element's range and make the resolved value stale (spurious `InvalidValue`, or the wrong level written). It now resolves the bound and writes under a single lock via a shared `write_element_value_locked` helper (the element's own min/max are trivially in range). Uses the real min/max, not a hardcoded 0/1, so the ranged dew-heater/fan still reach full scale.
- **ToupTek SDK wrapper: `close_shared` is now idempotent against a double-close** (AlpacaCore): a second close of an already-released handle fell through to `Toupcam_Close` unconditionally, which — if the opaque handle value had been recycled by a later open — could tear down an innocent holder. A live-handle set now makes `close_shared` a no-op for any handle not currently open, while still closing untracked `open_camera_by_index` handles (which are legitimately absent from the id map). Complements the ref-count underflow guard from the previous round.
- **ToupTek camera: exposure thread cleared `exposure_active_` off `readout_mutex_`** (AlpacaCore): the driver documents the invariant that `exposure_active_` only transitions under `readout_mutex_` (relied on by `set_readout_mode`/`set_gain`/`set_offset` to serialise a register write against the start of an exposure), and enforced it on the disconnect and stuck-exposure-deadline paths — but the exposure thread's own exit clears (abort/failed and natural-completion) stored `false` with no lock held. On arm64's weaker memory model that left the `readout_mutex_` TOCTOU story incomplete. Both exit paths now publish the false-transition under `readout_mutex_` (taken alone — the thread holds neither `mutex_` nor the SDK lock, and no join site holds `readout_mutex_`, so it respects lock order and cannot deadlock the joining thread).
- **ToupTek SDK wrapper: `close_shared` could underflow the ref count** (AlpacaCore): the shared-open release did `--open_count > 0` unconditionally, so a mispaired double-close would drive the count negative, fall through the erase, and `Toupcam_Close` a handle another driver still held open (undefined behaviour). It now only decrements when the count is `> 1` and treats `<= 1` as the final release, so the count can never underflow past the erase into a close-on-in-use handle.
- **ToupTek camera: open handle leaked when connect configuration threw** (AlpacaCore): after `open_camera_by_id`, the connect path made three unguarded SDK calls (`put_trigger_mode`, `get_serial_number`, `get_firmware_version`) before the `start_pull_mode` try/catch — so a throw there left `connected_` false (destructor skips the close) while the ref-counted open stayed at 1, permanently unbalanced. The next reconnect then bumped the count to 2 and handed back the same stale handle, so `Toupcam_Close` never fired. The whole post-open configuration is now wrapped in one guard that closes the handle on any throw.
- **ToupTek AFW: home wait could settle on a deceleration bounce** (AlpacaCore): once `wait_for_home` had seen the `-1` "moving" state, the very next non-negative read declared the wheel settled — so a brief valid-slot bounce during deceleration (before the home cycle finished) returned early, and the next `SetPosition` executed against an unhomed wheel. It now requires `kStableReads` (3) consecutive real-slot reads unconditionally (any `-1` resets the counter), costing at most ~300 ms on the nominal path.
- **ToupTek camera: sub-frame origin snap now documented** (AlpacaCore): the ROI origin snaps down to an even sensor-coordinate boundary (required by `Toupcam_put_Roi`, and mandatory on colour sensors to preserve Bayer phase), so at odd bin factors — notably bin 1 — an odd `StartX`/`StartY` resolves to the next lower even column/row. This intentional hardware-grid snap is now documented on `set_start_pos_locked` so callers needing an exact origin align to a 2-pixel boundary; the misordered `wait_for_home`/`expand_shorthand_locked` doc comments in the filter-wheel driver were also corrected.
- **ToupTek Thermal Switch routing test** (AlpacaHTTP): added a `(touptek, switch)` routing test for the `switchType: "thermal"` backend — it now round-trips `switchType` + `cameraIndex` through configure/list/remove and asserts an unknown `switchType` is rejected, closing the coverage gap next to the existing StellaVita and Player One thermal-switch tests.
- **ToupTek camera: 3×3 binning never delivered a frame** (AlpacaCore): with an odd bin factor the binned ROI span (`num × bin`) could be odd — e.g. a full-frame 3×3 on the ATR2600M asked `Toupcam_put_Roi` for a 4167-pixel-tall sensor ROI, which the SDK rejects because it requires even width/height. The exposure then hung until `ImageReady` timed out (2×2 and 4×4 always produce even spans, so only 3×3 failed). The driver now rounds the sensor-coordinate ROI span up to even (clamped to the sensor) and the offset down to even before `put_Roi`; digital binning floor-bins the padded span back to exactly the requested `num` output pixels, so the pixel buffer and reported `NumX`/`NumY` stay correct. For an edge-touching odd-bin sub-frame (where the padded span would run one pixel past the sensor), the even offset is shifted left/up by the overflow instead of shrinking the span, keeping the output count at `num`.
- **ToupTek Thermal Switch: use-after-close race** (AlpacaCore): `GetSwitchValue`/`SetSwitchValue` released the driver mutex before the SDK read/write, so a concurrent disconnect could `Toupcam_Close` the shared camera handle mid-call when the switch was the sole opener (undefined behaviour at the SDK level). Both now hold the driver mutex across the SDK call, serialising against `set_connected(false)`.
- **ToupTek Thermal Switch: out-of-bounds read after disconnect** (AlpacaCore): the metadata getters (`GetSwitchName`/`Description`, `MinSwitchValue`/`MaxSwitchValue`, `CanWrite`, `SetSwitchName`) checked `connected_` and then re-took the mutex to index `elements_`; a disconnect completing in that window clears `elements_` under the mutex, so the sentinel-bounded id (0..2) then indexed an empty vector. All lock-then-index paths now re-check the connection under the lock (`ensure_connected_locked`) before touching `elements_`.
- **Filter wheels: `Position` read/write use-after-close race** (AlpacaCore): the ToupTek, ZWO EFW and Player One filter-wheel drivers snapshotted the device handle/id under the mutex and then released it before the SDK `GetPosition`/`SetPosition` call, leaving a window where a concurrent disconnect could close the handle mid-call. All three now hold the driver mutex across the SDK call (matching the thermal switch), so a racing `set_connected(false)` — which closes the wheel under the same mutex — is serialised. Per the "fix shared patterns everywhere" rule.
- **ToupTek camera: `set_readout_mode` succeeded while disconnected** (AlpacaCore): on a camera with neither conversion-gain nor High Full Well support, the single "Normal" mode took an early return before any connection check, so `SetReadoutMode(0)` returned success while disconnected instead of throwing `NotConnected`. The range check still runs first (an out-of-range index is `InvalidValue` even while disconnected), then a connection check gates the early return.
- **ToupTek AFW config stripped on save** (AlpacaHTTP): `sanitize_device_config` is a strict allowlist, and the ToupTek non-switch branch only copied camera/focuser fields — so `filterwheelIndex`, `filterwheelId`, and `filterNames` were discarded on every save, resetting the wheel binding to index 0 and erasing user-customised filter names. The branch now splits out `filterwheel` and copies all three fields (matching the ZWO and Player One filter-wheel handling).
- **ToupTek camera: `set_readout_mode` applied CG and HFW non-atomically** (AlpacaCore): the two option writes were unsynchronised, so a concurrent `get_readout_mode` could observe an intermediate CG/HFW combination not in the enumerated specs and report the wrong mode. Both now hold a dedicated `readout_mutex_` across their paired option accesses.
- **ToupTek camera: `get_blacklevel_max` swallowed a bit-depth read failure** (AlpacaCore): a failed `TOUPCAM_OPTION_BITDEPTH` read silently defaulted to 8-bit, so a camera streaming in deep mode would report `OffsetMax` as 31 instead of its true deep-mode maximum. It now propagates the SDK failure like every other getter in the wrapper.
- **ToupTek camera: odd-bin ROI could deliver a short (black-edge) column/row** (AlpacaCore): the even-rounded binned span was clamped to the even sensor size, which on a sensor whose dimension is not a multiple of `2×bin` could leave the SDK's floor-binned output one pixel short of the requested `NumX`/`NumY` (a zero-filled edge column/row). The binned dimension limits are now derived from the *even* sensor size, so `ceil_even(num×bin)` always fits and floor-bins back to exactly `num` — the clamp is now provably unreachable. The ATR2600M (even dimensions) was unaffected either way.
- **ToupTek/ZWO/Player One filter wheel: `set_position` reported `DriverException` on concurrent disconnect** (AlpacaCore): all three checked slot-count validity before the disconnect sentinel, so a disconnect racing in (which clears the slot info first) surfaced a generic `DriverException` instead of `NotConnected`. `set_position` now checks the handle/id sentinel first, matching `get_position`. Per the "fix shared patterns everywhere" rule.
- **ToupTek filter-wheel/focuser opens are now reference-counted** (AlpacaCore): `open_filter_wheel_by_id`/`open_focuser_by_id` did raw `Toupcam_Open`/`Toupcam_Close` while only the camera path was ref-counted, so a second driver instance on the same device id got a null handle instead of sharing the open (and a camera plus its integrated autofocuser sharing an id would fail). All open-by-id/close paths now go through one ref-counted map (`open_shared_by_id`/`close_shared`).
- **ToupTek AFW config lost on web-UI edit** (AlpacaHTTP): editing a ToupTek camera/focuser re-ran the filter-wheel slot sync and blanked the displayed filter names/slots; and the filter-wheel form could not bind by SDK id (`filterwheelId` was never sent, so an id-bound wheel reverted to index 0 on save). The slot-UI sync is now gated on `deviceType === 'filterwheel'` (fixed for Player One too), and a "Filter Wheel ID (optional)" field (unique `touptekFilterwheelId` form name) now round-trips id binding like the focuser's id field.
- **ToupTek `enumerate_cameras` listed filter wheels and focusers as cameras** (AlpacaCore): `Toupcam_EnumV2` returns standalone AFW filter wheels and AAF focusers alongside cameras, but the camera enumeration pushed every entry. With an AFW and a camera both attached, `cameraIndex=0` could resolve to the filter wheel and the camera would fail to stream. It now skips entries flagged `TOUPCAM_FLAG_FILTERWHEEL`/`_AUTOFOCUSER` (mirroring the filter-wheel/focuser enumerations) and assigns a camera-only index. **Migration note:** because accessories no longer consume a camera slot, `cameraIndex` now counts cameras only — a user who previously worked around the bug by pointing past an accessory (e.g. `cameraIndex: 1` with an AFW enumerating first) must renumber to the camera-only index (usually `0`); an out-of-range saved index fails the connect with a clear "Camera index out of range" error.
- **ToupTek camera settings could be changed mid-exposure** (AlpacaCore): `SetReadoutMode` (and, by the same reasoning, `Gain`/`Offset`) wrote sensor registers without checking for an in-progress exposure, racing the live integration for a mixed-state or stalled frame with no error raised. All three now throw `InvalidOperation` while an exposure is active. Other camera drivers are unaffected — only ToupTek programs these registers at runtime.
- **ToupTek camera bin/full-frame dimension mismatch** (AlpacaCore): `set_bin_locked` and the connect-time default computed `NumX`/`NumY` from the raw sensor size while `start_exposure` bounds the ROI by the even sensor size, so on a hypothetical odd-width sensor exactly divisible by the bin factor a default full-frame exposure would throw "ROI size exceeds sensor dimensions." All three sites now use the even-sensor formula `(max & ~1) / bin`. The ATR2600M (even dimensions) was unaffected.
- **ToupTek focuser could not be bound by SDK id from the web UI** (AlpacaHTTP): the focuser-id input used the bare `name="focuserId"`, which collides with ZWO's focuser-id field (first in DOM order), so `formData.get('focuserId')` always returned ZWO's empty value and the ToupTek focuser silently registered by index 0. Renamed to the unique `touptekFocuserId` (read accordingly in the submit handler) — the same FormData-collision fix already applied to `switchType`/`filterwheelId`. Per the "fix shared patterns everywhere" rule.
- **ToupTek camera: exposure dirty-flag reset race** (AlpacaCore): the exposure thread cleared `format_dirty_`/`roi_dirty_` *after* its SDK `put_roi`/`put_binning`, so a concurrent `set_num_x`/`set_roi` that re-dirtied the flags in that window was silently overwritten to false and the next exposure reused a stale ROI/format. The flags are now cleared at snapshot time under the same lock, and re-marked dirty if the SDK reconfigure fails.
- **ToupTek camera: `get_readout_mode` returned 0 while disconnected** (AlpacaCore): the single-mode early return preceded any connection check, violating the ASCOM contract that properties throw `NotConnected` when disconnected (the setter already guarded this). It now calls `ensure_connected()` first.
- **ToupTek drivers: connection-thread teardown race** (AlpacaCore): a `connect()` racing the destructor could spawn a `connection_thread_` that outlived the object, leaving it unjoined at destruction (`std::terminate`). The ToupTek camera, filter-wheel, focuser, and thermal-switch drivers now set a `shutting_down_` flag under `connection_mutex_` before joining, and `start_connection_task` refuses to spawn once it is set.
- **Filter wheel single-token name shorthand was dead when connected** (AlpacaCore): `set_names` validated the name count before the `"LRGB" → L,R,G,B` shorthand expansion, so on a connected wheel `set_names({"LRGB"})` threw `InvalidValue` before it could expand. The expansion is now factored into a shared `expand_shorthand_locked` applied before validation, in both the ToupTek and ZWO filter-wheel drivers. Per the "fix shared patterns everywhere" rule.
- **ToupTek camera: dirty flags lost when ROI validation throws** (AlpacaCore): the dirty-flag clear was moved to snapshot time in the prior fix, but it ran *before* the ROI bounds validation — so an invalid sub-frame that threw `InvalidValue` cleared `format_dirty_`/`roi_dirty_` without ever spawning the thread that restores them, and the next valid exposure skipped `put_binning`/`put_roi`. The clear now runs at the end of the snapshot's locked block, after all validation, preserving the race fix while keeping the flags dirty on a validation throw. The failure catch also re-marks only the reconfigure stage that did not complete, avoiding a spurious stream restart.
- **ToupTek camera: geometry setters writable mid-exposure** (AlpacaCore): `set_bin_x`/`set_bin_y`, `set_num_x`/`set_num_y`, and `set_start_x`/`set_start_y` could mutate binning/ROI state during a live integration, leaving the in-flight exposure's captured geometry inconsistent with the next frame's buffer sizing. All three locked helpers now call `ensure_not_exposing()` **under `readout_mutex_`** (lock order `mutex_ → readout_mutex_`), so the check is atomic with the geometry mutation and with `start_exposure` publishing `exposure_active_` — a bare pre-lock check was a TOCTOU (matching `set_gain`/`set_offset`/`set_readout_mode`).
- **Filter wheel `set_names` left a partial array on a validation throw** (AlpacaCore): the shorthand-expansion reorder assigned `filter_names_` before validating the count, so a wrong-length `Names` PUT to a connected wheel left `filter_names_` with the wrong number of entries (and skipped the default-name fill) until a correct call. `set_names` now expands and validates on a local copy, committing only on success, in both the ToupTek and ZWO filter-wheel drivers (`expand_shorthand_locked` takes the target vector by reference).
- **ToupTek AFW: `Connected` reported before the home cycle finished** (AlpacaCore): `set_connected(true)` sent the home command (`FILTERWHEEL_POSITION = -1`) and immediately reported connected, but the firmware takes ~1.5 s to home; a `SetPosition` arriving in that window aborted the home and left the slot reference unknown (moves then land on wrong slots). Connect now polls the wheel position until it settles to a non-negative slot (or times out) before reporting connected, releasing the driver mutex during the poll so an eager `GetName`/`GetUniqueId` (NINA calls these at enumeration) doesn't block for up to 6 s. The poll settles on either signal — the position transitioning through `-1` (moving) and back to a real slot, or a real slot held stable across several polls — so it neither returns before the move began nor stalls the full timeout on hardware that homes faster than one poll interval. If neither signal is seen within the timeout (obstructed wheel / stuck firmware) the connect now **fails** with `DriverException` rather than reporting `Connected` on a wheel whose slot reference is unknown.
- **ToupTek AFW: stale slot count after disconnect** (AlpacaCore): `set_connected(false)` left `slot_count_` at its last-connected value, so a `Names`/`FocusOffsets` write after a disconnect (e.g. swapping a 5-slot wheel for a 7-slot before reconnecting) was validated against the stale count and rejected with `InvalidValue`. Disconnect and failed-connect now reset `slot_count_`/`info_`/`handle_` to the clean disconnected state.
- **ToupTek camera: `SetReadoutMode` on a Normal-only camera skipped the mid-exposure guard** (AlpacaCore): the single-mode early return returned before the exposure check, so a mid-exposure ReadoutMode change was accepted (a no-op, but a silent ASCOM-contract violation). It now takes `readout_mutex_` and throws `InvalidOperation` while exposing, consistent with the write path.
- **Filter-name shorthand could send the wrong count from the web UI** (AlpacaHTTP): `parseFilterNamesInput` split a single all-caps token (`LRGB`) into per-character names *client-side without knowing the slot count*, so on a 5-slot wheel it sent 4 names — silently padded (pre-connect) or rejected (post-connect). Submit now sends the raw token and lets the slot-count-aware C++ `expand_shorthand_locked` expand it; the client-side split is kept only for the live slot-preview UI.
- **ToupTek Thermal Switch: `MaxSwitch` was unstable across connect state** (AlpacaCore): it reported `kMaxThermalElements` (3) while disconnected and the real element count (1–2 on a heater-only camera) once connected. It now caches the probed count and reports it while disconnected too, so `MaxSwitch` stays stable after the first connect (only the very first pre-connect query returns the worst-case bound).
- **ToupTek Thermal Switch: `GetSwitch` read value and min non-atomically** (AlpacaCore): it called `GetSwitchValue` then `GetMinSwitchValue` across two separate lock acquisitions, so a disconnect landing in the gap surfaced a spurious `NotConnected` after the value had already read successfully. Both are now read under a single lock (the per-element SDK read is factored into a shared locked helper).
- **ToupTek camera: detached pulse-guide timer was a use-after-free** (AlpacaCore): `PulseGuide` spawned a *detached* thread that slept for the pulse duration and then wrote `pulse_guiding_`; if the driver was destroyed mid-pulse (e.g. a NINA reconnect during a 2 s pulse) that write hit freed memory. The timer is now a joinable member thread with a cancel flag + condition variable, cancelled and joined in the destructor (and superseded cleanly when a new pulse starts).
- **ToupTek camera: readout/gain/offset could use a handle closed by a concurrent disconnect** (AlpacaCore): `get`/`set_readout_mode`, `set_gain`, and `set_offset` snapshot the handle, then do the SDK call under `readout_mutex_` — but `set_connected(false)` closed the handle without taking that lock, so a disconnect landing in the gap left them calling the SDK on a closed handle. `set_connected(false)` now holds `readout_mutex_` (after `mutex_`) across the close and clears `connected_` under it, and each of those methods re-checks `ensure_connected()` under `readout_mutex_` before the SDK call — so a racing disconnect yields `NotConnected` instead of a stale-handle call.
- **ToupTek camera: sensor-register setters TOCTOU with `start_exposure`** (AlpacaCore): the mid-exposure guard was checked before acquiring `readout_mutex_`, so a concurrent `start_exposure` on another connection could begin integrating between the check and the register write. `set_readout_mode` (CG/HFW), `set_gain`, and `set_offset` (black level) now all check `ensure_not_exposing()` *under* `readout_mutex_` and hold it across the write, and `start_exposure` publishes `exposure_active_` under the same mutex — making every runtime sensor-register write mutually exclusive with a live integration.
- **ToupTek camera: `StopExposure`/`AbortExposure` blocked for the full exposure time** (AlpacaCore): `stop_exposure` set `exposure_active_=false` and joined the exposure thread but never called `Toupcam_Stop`, so the thread stayed parked in `Toupcam_WaitImageV4` until its timeout — aborting a 10-minute frame took ~10 minutes. Both stop paths (`stop_exposure` and the internal `stop_exposure_thread`) now call `stop()` to unblock the wait before joining (as the disconnect path already did), and force `format_dirty_` (plus `roi_dirty_` as a defensive safety net) so the next exposure re-inits the halted pull-mode stream and re-applies the ROI. The `stop()` call is made under `mutex_` (so a concurrent disconnect cannot `Toupcam_Close` the handle in the gap — use-after-close), and `exposure_active_` is left for the exposure thread to clear on its own exit rather than pre-cleared before the join (pre-clearing would let a concurrent `set_readout_mode`/`set_gain` write registers while the frame is still live). `stop_exposure_thread` moved ahead of the ROI snapshot in `start_exposure` so that re-init is reflected in the same exposure.
- **ToupTek camera: deadline path cleared `exposure_active_` off `readout_mutex_`** (AlpacaCore): the `get_camera_state` stuck-exposure deadline stored `exposure_active_=false` under `mutex_` only, breaking the invariant that the flag changes solely under `readout_mutex_` (which `set_readout_mode` relies on). The deadline branch now also takes `readout_mutex_` (lock order `mutex_ → readout_mutex_`).
- **Serial protocol wrappers: partial writes and unchecked `fcntl`** (#94): POSIX `write()` can satisfy only part of a payload (or be interrupted by `EINTR`), and `fcntl(F_GETFL)` can fail and return `-1` — feeding that into `flags & ~O_NONBLOCK` could leave the fd non-blocking and spin a reader thread at 100% CPU. Both patterns (first fixed in the WandererCover wrapper) are now corrected across the Gemini, iOptron, SynScan, Celestron, ZWO-mount and Bisque wrappers via a shared `alpacacore/util/serial_io.h` (`write_all()` full-write loop + `clear_nonblocking()` checked fd flag clear). Per the "fix shared patterns everywhere" rule.
- **Router device registration is exception-safe** (#95): every vendor block in `AlpacaHTTP/src/http/router.cpp` now registers drivers via `std::shared_ptr<AlpacaDriver>(std::move(ptr))` instead of `std::shared_ptr<AlpacaDriver>(ptr.release())`. The old form orphaned the released raw pointer (leak) if the `shared_ptr` control-block allocation threw `std::bad_alloc`. 23 call sites converted (the WandererCover block already used the safe form).

### Changed
- **Dead stores flagged by the scan-build evaluation removed** (#106): three copy-pasted `parse_string` helper lambdas in `router.cpp` that no code ever called, a `location_written = true` after its final read, a mid-cycle `last_level = 1` PWM-cache store that the unconditional end-of-cycle store made unreadable (ASIAIR Plus RK3568 wrapper), and `logging_adapter.cpp`'s persisted-level `switch` now lets the `"INFO"` initializer serve as the `Info` case. No behavior changes.
- **Switch DeviceState consolidated into the `SwitchDriver` base** (#107): `SwitchDriver` now provides the Platform 7 `get_device_state()` once, inline in the header (getter-based: `GetSwitchN`/`GetSwitchValueN`/`StateChangeCompleteN` per id plus a `TimeStamp`), and all seven per-vendor overrides were removed (ToupTek thermal + StellaVita, ZWO dew heater + ASIAIR + ASIAIR Plus RK3568, Player One, iOptron iMate). The four wrapper-direct overrides (iOptron, ASIAIR, ASIAIR Plus RK3568, StellaVita) previously read `wrapper_.get_value()` directly, bypassing the public getters — the exact DeviceState↔GET desync AGENTS.md warns against; their DeviceState values are unchanged (verified against the getter implementations) but now gain a `TimeStamp` and stay consistent by construction. Disconnected DeviceState reports only the `TimeStamp` (previously an empty bag). ConformU V4 re-runs on the affected hardware are tracked on #107.
- **AGENTS.md structural cleanup** (#103): general rules de-duplicated out of the Vendor-Specific Notes into general sections (GPIO power-switch / soft-PWM rules, camera ROI alignment, FilterWheel semantics, config round-trip pointers to the canonical "Enumeration index fields"), `Units and Behavior Conventions` moved up beside `Language, Style, and Safety`, the structured-`Value`-is-JSON contract rehomed under Alpaca Protocol Conformance, a SemVer / CHANGELOG bump policy documented, and lab-notebook narratives trimmed to their reusable one-liners. Also syncs the `VERSION` file to the `3.0.0` this section already carries — the PR #99 review commit that ruled the `cameraIndex` enumeration change breaking bumped the CHANGELOG label but missed `VERSION`.
- **Filter-name shorthand expansion is now stricter** (AlpacaCore + AlpacaHTTP): a single delimiter-less filter name whose length equals the slot count is expanded into per-slot single characters (`LRGB` → `L,R,G,B`) only when it contains no lowercase letters, so ordinary names like `Clear` or `Ha_NB` are no longer silently exploded. Applied consistently across the shared web-UI parser and the ZWO / ToupTek filter-wheel drivers (Player One never expanded), with a note added to the web-UI help text. Per the "fix shared patterns everywhere" rule.
- **Filter-name help text calls out the all-uppercase edge case** (AlpacaHTTP): the ZWO and ToupTek filter-name tooltips now note that an all-caps word like `DARK` on a 4-slot wheel expands to `D, A, R, K` and that a trailing comma keeps it as one name; the Player One tooltip no longer claims expansion (that driver never expands shorthand).
- **Auto-detect failure message is platform-aware** (#96): serial port enumeration is POSIX-only, so on Windows the `enumerate_*_ports()` helpers always return empty — which previously surfaced a misleading "No <device> detected on any serial port" at connect rather than indicating auto-detect is unavailable. A shared `alpacacore/util/auto_detect.h` helper now reports "Auto-detect is not supported on this platform — configure an explicit serial port" on platforms without enumeration, used by the WandererCover, Gemini, SynScan, Celestron and iOptron auto-detect drivers. AlpacaBridge still targets Linux arm64 only; this only clarifies the Windows scaffolding path.

<details>
<summary><strong>[2.1.0] - 2026-06-23</strong></summary>

### Added
- **WandererAstro WandererCover V4 CoverCalibrator driver** (AlpacaCore): the project's first CoverCalibrator driver — a motorized dust cover plus EL flat panel over a CH340 USB-serial link (19200 8N1). Protocol wrapper with a background reader thread that parses the device's continuously-streamed `A`-delimited status frame, fire-and-forget commands (open `1001`, close `1000`, brightness `1`–`255`, off `9999`), and CH340/CH341 auto-detection (`/dev/serial/by-id` → `WandererCoverV4` status match). Cover state is inferred from the streamed angle vs. the configured open/close set points (±10° tolerance, INDI-aligned); calibrator state/brightness are tracked synchronously so reads are correct the instant after `CalibratorOn`/`CalibratorOff`. `HaltCover` has no hardware command, so it stops the driver's move-tracking (cover finishes its travel mechanically) rather than throwing `NotImplemented`, per the ASCOM cover-capable-device contract. Validated with ConformU 4.3.0 on real hardware (WandererCover V4 Pro, firmware 20250504, Linux arm64): 0 errors, 0 issues, 0 timing issues.
- **WandererAstro CoverCalibrator support** (AlpacaHTTP): router registration and instantiation (auto-detect / serial), config sanitization, web UI vendor + Cover Calibrator device type with Auto-detect/Serial connection fields and per-`(vendor, deviceType)` index auto-numbering, and a routing/config-persistence test. The generic CoverCalibrator HTTP dispatch already existed; this is its first vendor.
- **CoverCalibrator DeviceState** (AlpacaCore): added a default Platform-7 `get_device_state()` to the `CoverCalibratorDriver` base (Brightness, CalibratorState, CoverState, CalibratorChanging, CoverMoving, TimeStamp) so all future CoverCalibrator vendors inherit it.
- **WandererAstro unit tests** (AlpacaCore): 8 Catch2 test cases, 46 assertions.

### Fixed
- **Vendor index fields could not be set to anything but 0** (AlpacaHTTP web UI): every camera index input shared `name="cameraIndex"` (and three focuser inputs shared `name="focuserIndex"`). Hidden vendor sections still submit, so `formData.get('cameraIndex')` always returned the first such field in DOM order — ZWO's — and the value typed into a Player One / QHY / SVBONY / ToupTek (or Gemini focuser) field was discarded, persisting `0`. Adding a second device of one of those vendors silently collided on index 0, and the index could not be corrected in the form. Each non-ZWO index input now has a unique vendor-prefixed `name` (`playerOneCameraIndex`, `qhyCameraIndex`, `svbonyCameraIndex`, `touptekCameraIndex`, `touptekFocuserIndex`, `geminiFocuserIndex`) and the submit handler reads it; ZWO keeps the canonical names. Same FormData-collision class already documented for filter names.
- **Camera index did not auto-increment for non-ZWO vendors** (AlpacaHTTP web UI): auto-numbering was hardcoded to ZWO's camera field, so a second Player One/QHY/SVBONY/ToupTek camera (and ZWO focuser/filterwheel/rotator) defaulted to index 0 instead of the next free value. Replaced the ZWO-only logic with a declarative `INDEX_FIELDS` registry that drives per-`(vendor, deviceType)` auto-increment, manual-edit tracking, and the edit-mode reset for all index-addressed fields. The index is scoped per vendor SDK (each enumerates from 0 independently, so a ZWO camera and a Player One camera are both index 0); this is distinct from the Alpaca device number, which already auto-assigns per device type.

### Changed
- **`AGENTS.md` + `/driver-build` skill**: documented the two rules a new index-addressed vendor must follow — a unique vendor-prefixed form-field `name` (to avoid the FormData collision above) and an `INDEX_FIELDS` registry entry (for auto-numbering) — with the device-number-vs-index distinction spelled out, so this doesn't resurface when the next camera/focuser is added.

</details>

<details>
<summary><strong>[2.0.2] - 2026-06-16</strong></summary>

### Changed
- **`/commit` skill**: now also keeps `docs/architecture.md` current — Step 4 instructs it to update the **Vendor drivers** table (device types, wrapper type, status) and the `external/` SDK listing whenever a commit adds or changes vendor/driver/SDK support, deriving the truth from the source tree rather than memory.
- **`docs/architecture.md`**: brought the **Vendor drivers** table up to date with all shipped drivers that had drifted out — Player One (FilterWheel, Switch), ZWO (ASIAIR-power Switch, AM-mount Telescope), ToupTek (AAF Focuser, StellaVita Switch), and iOptron (iMate PowerBox Switch); ZWO and ToupTek wrapper type noted as SDK + protocol.

### Fixed
- **Configure tab kept a previous device's settings** (AlpacaHTTP web UI): the device form was only cleared on a successful submit, so after editing or partially filling a device, opening the **Configure** tab to add a new one still showed the old vendor, indexes, ports, filter names, and the "Edit Device" / "Update Device" mode — and submitting from that leaked edit state would remove-and-re-add the wrong device. Opening the Configure tab fresh now resets the form to a clean "Add Device" state via a new `resetDeviceForm()` (native reset, edit mode cleared, vendor sub-sections re-toggled, ZWO/Player One filter-wheel slot UIs resynced); the edit flow switches tabs with `showTab('configure', { preserveForm: true })` so the populated form it just built survives. `showTab` now marks the active tab button by name instead of relying on the global `event`, so the post-submit `showTab('devices')` no longer depends on a stale `event.target`.

</details>

<details>
<summary><strong>[2.0.1] - 2026-06-14</strong></summary>

### Added
- **Vendored ASCOM Alpaca API spec** (docs): the upstream OpenAPI YAML behind https://ascom-standards.org/api/ is now committed at `docs/AlpacaDeviceAPI_v1.yaml` (OpenAPI 3.1.1, MIT-licensed) as the in-repo source of truth for driver development.

### Changed
- **`/driver-build` skill**: added Step 0 that verifies the vendored ASCOM Alpaca spec is current against ascom-standards.org on every run (diffs version/endpoints, re-downloads on major changes); Step 3 now drives off the vendored `docs/AlpacaDeviceAPI_v1.yaml` to follow the API exactly; reinforced cross-driver consistency (filter wheels match ZWO EFW / Player One Phoenix setup).
- **`/commit` skill**: documented the project Semantic Versioning policy (driver=minor, fix/docs=patch, breaking=major) with cumulative carry-forward; `/commit` now only sets the CHANGELOG `UNRELEASED` heading and never edits `VERSION` or the README badge.
- **`/submit-pr` skill**: verifies the UNRELEASED version matches the versioning policy for the branch, and asks whether the PR is cutting a release — offering to bump the `VERSION` file and `README.md` version badge (and date the CHANGELOG entry) when it is.
- **Collapsible CHANGELOG**: every released version section is now wrapped in a `<details>`/`<summary>` block (same pattern as `SUPPORTED-DRIVERS.md`) so the file folds to a scannable list of versions while the current/unreleased version stays expanded. `scripts/changelog_to_deb.py` now parses the collapsed `<summary>` heading form too, so Debian changelog generation is unaffected. The `/commit` skill documents the convention so new entries keep it.

### Fixed
- **GPIO Switch drivers failed to connect under the systemd service** (debian/packaging): the `.deb` postinst added the `alpacabridge` service user to `plugdev`/`dialout`/`input` but not `gpio`, so the libgpiod-based Switch drivers — iOptron iMate PowerBox, ToupTek StellaVita, and ZWO ASIAIR Pro / Plus (Pi CM4) — got `Permission denied` opening `/dev/gpiochip*` (owned `root:gpio` by the OpenAstro images' udev rule) and Switch connect failed with "Failed to open GPIO chip ...". The postinst now also runs `usermod -aG gpio alpacabridge` (guarded by `getent group gpio`, matching the existing group grants); `dh_installsystemd` restarts the service after upgrade so the new membership takes effect without a manual restart. Only affected the packaged service user — dev runs via `build_and_run.sh` already add the invoking user to `gpio`.

</details>

<details>
<summary><strong>[2.0.0] - 2026-06-13</strong></summary>

### Added
- **ASCOM Platform 7 interface versions + compliant DeviceState** (AlpacaCore): every driver now advertises its Platform 7 interface version and returns a spec-compliant `DeviceState` (the `connect`/`connecting`/`devicestate`/`disconnect` endpoints were already wired in AlpacaHTTP). `InterfaceVersion` bumped to ICameraV4 (4), ITelescopeV4 (4), IFocuserV4 (4), IRotatorV4 (4), and IObservingConditionsV2 (2); FilterWheel and Switch already reported their Platform 7 versions (IFilterWheelV3 / ISwitchV3). The per-vendor `DeviceState` overrides — which emitted non-standard names (`Connected`, `CoolerOn`) and omitted the `TimeStamp` — are replaced by one spec-compliant implementation in each device base class (`CameraDriver`, `TelescopeDriver`, `FocuserDriver`, `RotatorDriver`, `ObservingConditionsDriver`, `FilterWheelDriver`). Each base builds the operational-property list by calling the device's own property getters inside a try/catch (a property whose getter throws — `AlpacaException` or any unwrapped vendor `std::exception` — is omitted rather than failing the whole call), so `DeviceState` always agrees with the matching GET endpoint, which is the consistency ConformU verifies (note: the response is no longer an atomic snapshot — each getter locks separately, which the ASCOM spec permits); a shared inline `device_state_timestamp()` helper appends the ISO 8601 `TimeStamp`. The base `get_device_state()` and the timestamp helper are defined inline so the device-class vtables stay weak and the per-vendor static libraries link without a base-library ordering dependency. Telescope omits `UTCDate` (optional "if known") to avoid format drift versus the `/utcdate` endpoint. Camera DeviceState reports `CameraState`, `CCDTemperature`, `CoolerPower`, `HeatSinkTemperature`, `ImageReady`, `IsPulseGuiding`, `PercentCompleted`. Per-driver `InterfaceVersion` unit assertions updated, disconnected-state tests updated to the new contract, and DeviceState regression tests added (TimeStamp present, no `Connected`/`CoolerOn`, all names valid, and omit-on-throw covering unwrapped vendor exceptions). Full AlpacaCore suite green (179 tests, vendors ON). All device types are ConformU-validated on Linux arm64 under their Platform 7 interface versions — Camera (ICameraV4), Telescope (ITelescopeV4), Focuser (IFocuserV4), Rotator (IRotatorV4), ObservingConditions (IObservingConditionsV2), FilterWheel (IFilterWheelV3), and Switch (ISwitchV3) — each reporting 0 errors, 0 issues, 0 timing issues against the new `DeviceState` and async `Connect`/`Disconnect` lifecycle.
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
- **Player One Phoenix Wheel FilterWheel Driver** (AlpacaCore): new `FilterWheelDriver` at IFilterWheelV3 for the Player One Phoenix filter wheel (PW5/PW7/PW8), built over the **separate** Player One FilterWheel SDK v1.2.3 (`libPlayerOnePW` — its own C API, unrelated to the camera SDK) via a dedicated 4th-file SDK wrapper (`playerone_pw_wrapper`: singleton, mutex-serialized, reference-counted open/close, `PWErrors` → `AlpacaException` mapping). The Phoenix Wheel stores per-slot filter aliases and focus offsets on the device itself; the driver seeds `Names`/`FocusOffsets` from the wheel at connect, with config-supplied `filterNames` taking precedence (never written back to the wheel). ASCOM `Position` returns −1 while moving — the wrapper checks `POAGetPWState` and maps the SDK's `PW_ERROR_IS_MOVING` read window instead of throwing. SDK vendored arm64-only under `external/PlayerOne/PlayerOne_FilterWheel_SDK_Linux_V1.2.3/` (arm32/x64/x86 libs, static libs, examples, and Python/C# bindings removed; the udev rules are byte-identical to the camera SDK's already-installed copy). `libPlayerOnePW.so` ships in the `.deb` (`debian/rules`) and via `build_and_run.sh` / `install_alpaca_service.sh`.
- **Player One Phoenix Wheel Device Support** (AlpacaHTTP): router accepts `vendor=playerone deviceType=filterwheel` with `filterwheelIndex` and optional `filterNames`; config sanitization persists both (and keeps `cameraIndex` from leaking into a filterwheel config). Web UI gains Player One as a FilterWheel vendor with device-type-aware sub-sections (camera vs. filterwheel), a wheel-index field, and the standard slot UI with the Phoenix lineup (5 slots (PW5) / 7 slots (PW7) / 8 slots (PW8) + Custom). Form fields use vendor-prefixed names (`playerOneFilterwheelIndex`, `playerOneFilterNames`) to avoid FormData collisions with ZWO's same-purpose hidden fields.
- **Player One Phoenix Wheel Unit Tests**: 6 test cases, 31 assertions covering defaults, device metadata, `NotConnected` error codes on position operations, unsupported actions/commands, Platform 7 DeviceState while disconnected, default "Filter N" name fill-in, device number assignment, and unique IDs.
- **Player One Phoenix Wheel PW8 ConformU 4.3.0 validated** on Linux arm64 at **IFilterWheelV3**: 0 errors, 0 issues, 0 timing issues over USB, exercising all 8 positions with up/down/random move sequences (16 moves total). Wide timing margins throughout — slowest member 0.296 s vs the 1.0 s STANDARD target, all FAST members ≤ 0.042 s. AB TRACE log clean for the test window. Results saved to `AlpacaCore/conformu/Player One/PW8/Linux-arm64.txt`; `SUPPORTED-DRIVERS.md` gains a Player One FilterWheel section with ✓.
- **Player One Thermal Switch Driver** (AlpacaCore): new `SwitchDriver` ("Player One Thermal Switch", ISwitchV3) exposing a cooled Player One camera's dew (lens) heater and radiator fan (`POA_HEATER_POWER` / `POA_FAN_POWER`, percent, SDK-reported ranges) as multi-value switch elements — the sliders NINA-style clients use for runtime control. The element list is probed per model at connect (heater and/or fan); while disconnected `MaxSwitch` reports the potential count (2) and out-of-range IDs throw `InvalidValue` even when disconnected, matching the ZWO dew-heater switch contract. Connecting against a camera with neither control (uncooled guide cams) fails with `NotImplemented`. The camera SDK wrapper's `open_camera`/`close_camera` are now **reference-counted** (mirroring `zwo_sdk_wrapper`) so the switch and camera devices share one `POAOpenCamera` handle — the camera physically closes only when the last user disconnects. Heater/fan are deliberately **runtime-only**: a connect-time `heaterPower`/`fanPower` camera config was implemented and removed in review (a persisted "heater on" set in December would silently re-apply at every connect months later — wasted power, heat fighting the TEC, no client visibility); on power-up the camera keeps its firmware defaults and turning the heater on is an explicit per-session act. Cooling stays exclusively on the standard Camera interface (`CoolerOn`/`SetCCDTemperature`), same single-owner model as ZWO. Hardware-verified on a **Uranus-C PRO (IMX585)**: cooler reached and held −10 °C (first live run of the existing Player One cooler path on cooled hardware), fan slider responds audibly end-to-end, and the dew heater was proven by calorimetry — heater 0→100 % at a held −10 °C target raised steady-state cooler power ~34 %→~44 %, symmetric on heater-off.
- **Player One Camera heater/fan Actions** (AlpacaCore): the camera driver gains `SupportedActions` — `GetHeaterPower`, `SetHeaterPower`, `GetFanPower`, `SetFanPower` (case-insensitive per the ASCOM Action contract) — for scripted runtime control of the same two controls. Non-integer or out-of-range parameters throw `InvalidValue` against the SDK-reported range; models without the control throw `NotImplemented`; unknown action names still throw `ActionNotImplemented`.
- **Player One Thermal Switch Device Support** (AlpacaHTTP): router accepts `vendor=playerone deviceType=switch` with `cameraIndex` (the cooled camera whose heater/fan the switch controls); config sanitization persists `cameraIndex` for both camera and switch device types. Web UI gains Player One as a Switch vendor with a device-type-aware sub-section (switch camera-index field, vendor-prefixed form name to avoid FormData collisions).
- **Player One Thermal Switch Unit Tests**: 8 test cases / 46 assertions covering defaults (potential element count while disconnected), device metadata, `NotConnected` error codes, unsupported actions/commands, ID-validation-before-connection-check ordering (`InvalidValue` wins while disconnected), async-unsupported error codes, the disconnected state machine, and a hardware-independent connect-failure case (absurd camera index, so a dev machine with a live camera at index 0 never has the test open the real device). The camera Actions test is expanded (supported-actions list, case-insensitivity, `ActionNotImplemented` vs `NotConnected` codes), and AlpacaHTTP routing tests cover camera + switch registration and `cameraIndex` persistence.
- **Player One Uranus-C PRO Camera + Thermal Switch ConformU 4.3.0 validated** on Linux arm64 (Raspberry Pi) at **ICameraV4** and **ISwitchV3**: both 0 errors, 0 issues, 0 timing issues, first attempt. Camera run exercised real exposures, the cooler surface, binning/ROI, and pulse guiding (slowest member `ImageArrayVariant` 3.08 s vs the 600 s EXTENDED target); switch run exercised both heater and fan elements through their full ranges (slowest member 38 ms vs the 100 ms FAST target). The only AB-log errors in the window were ConformU's deliberate `ImageArray`-before-exposure negative tests. Reports saved to `AlpacaCore/conformu/Player One/Uranus-C PRO/Linux-arm64.txt` and `AlpacaCore/conformu/Player One/Uranus-C PRO Thermal Switch/Linux-arm64.txt`; `SUPPORTED-DRIVERS.md` gains the Uranus-C PRO Camera row, a Player One Switch section with ✓, and the camera notes upgrade cooling from "untested on cooled hardware" to hardware-validated.

- **Log retention auto-cleanup** (AlpacaHTTP): new `logging.retention_days` config key (default 90, 0 = forever) and `ALPACAHTTP_LOG_RETENTION_DAYS` env var. Daily files older than retention are auto-deleted on startup and again on every day-rollover inside the file sink. Today's active file is never pruned.
- **Log level persistence** (AlpacaHTTP): log level changes made via `POST/PUT /management/v1/loglevel` (i.e. the web portal toggles) now survive a server restart. The chosen level is written to `config/runtime_state.json` and reapplied on next startup, overriding `default.yaml`'s `logging.level`. Removing the state file falls back to the YAML default.
- **On-disk logging** (AlpacaHTTP): server now writes every log line to a daily file `/var/log/AlpacaBridge/alpacabridge-YYYY-MM-DD.log` in addition to stderr and the in-memory buffer. Thread-safe append sink with automatic day-rollover and a writability probe that falls back to `$XDG_STATE_HOME/AlpacaBridge/logs` (or `~/.local/state/AlpacaBridge/logs`) when the configured directory is not writable. Configurable via new `logging.directory` and `logging.file_enabled` keys in `config/default.yaml` and env vars `ALPACAHTTP_LOG_DIRECTORY` / `ALPACAHTTP_FILE_LOGGING`.
- **Log file management API** (AlpacaHTTP): three new management endpoints — `GET /management/v1/logfiles` (list daily files with size/modified time), `GET /management/v1/logfiles/{name}[?download=1]` (read inline or as attachment), `DELETE /management/v1/logfiles/{name}` (delete). Filenames are validated against the `alpacabridge-YYYY-MM-DD.log` pattern to prevent path traversal. HTTP parser extended with `DELETE` method support.
- **Web portal log file panel** (AlpacaHTTP): new "Stored log files" section under Logging shows each daily file with size and modified time, plus per-row **View** (dark-theme inline viewer), **Download**, and **Delete** (confirm-prompted) buttons. Auto-loads on page open and on the Refresh button.
- **Download All Logs / Delete All Logs** (AlpacaHTTP): `GET /management/v1/logfiles?download=1` returns every stored daily log file as a single gzip archive (chronological order, `===== filename =====` separators between days; oversized files are skipped with an inline note per the existing 10 MiB read cap), and `DELETE /management/v1/logfiles` deletes all stored files in one call (returns `DeletedCount`). The archive is capped at 200 MiB to bound peak memory on small SBCs — the newest files are kept, and a leading note in the archive reports how many older files were omitted. The web UI's Stored log files header now has **Refresh | Download All Logs | Delete All Logs** (delete is confirm-prompted); the old standalone Download Logs button (today's file only) is replaced, and the archive saves as `alpacabridge-logs-YYYY-MM-DD.txt.gz`. Both endpoints verified live against a scratch server (gzip validated with `zcat`, delete-all left an empty directory). New build dependency: **zlib** (`find_package(ZLIB)` in AlpacaHTTP, `zlib1g-dev` in debian/control Build-Depends and the CI apt lists).
- **Deb packaging** (debian/): `alpacabridge.service` adds `LogsDirectory=AlpacaBridge` so systemd creates and chowns `/var/log/AlpacaBridge` on every service start; `alpacabridge.postinst` also pre-creates the directory for non-systemd execution.
- **Dev install scripts**: `install_alpaca_service.sh` mirrors the systemd `LogsDirectory=` line and pre-creates `/var/log/AlpacaBridge` with the invoking user's ownership; `build_and_run.sh` unconditionally creates and chowns `/var/log/AlpacaBridge` on Linux so dev runs write to the standard path instead of the home-dir fallback.

### Changed
- **build_and_run.sh startup banner shows network URLs**: the "AlpacaHTTP is running" message now prints the host's name and each IPv4 LAN URL (`hostname -I`, one per interface) alongside `http://localhost:6800/`, so the web UI can be opened from another machine without first looking up the device's IP. IPv6 addresses are skipped (they need URL brackets and nobody types them).
- **CI clang-tidy compile DB now built with all vendors ON** (`.github/workflows/ci.yml`, `scripts/ci_preflight.sh`): vendor driver/wrapper sources had no entry in the previous vendor-neutral compile DB, so clang-tidy interpolated a compile command missing the vendored SDK include dirs and reported a spurious `clang-diagnostic-error` (e.g. `'PlayerOnePW.h' file not found`) on any PR touching SDK-based vendor code. Nobody had hit this before — every SDK vendor predates the clang-tidy gate, and the drivers added since were GPIO-based (whose `<gpiod.h>` resolves because CI installs libgpiod v2 into the default search path). Both CI and the local pre-flight now configure with `ALPACACORE_ENABLE_ALL_VENDORS=ON` so vendor sources are analyzed with their real compile commands.
- **FilterWheel slot UI is now a reusable component required for every filterwheel vendor** (AlpacaHTTP): the slot-count select + per-slot filter name dropdowns + advanced names textarea — previously six functions hard-wired to ZWO's element IDs — are refactored into a `createFilterwheelSlotUI({...})` factory instantiated per vendor (ZWO and Player One today). ZWO behavior is unchanged. New policy documented in `AGENTS.md` ("FilterWheel vendors — required web UI") and enforced by `/driver-build` (new Question 4b asks what slot counts the manufacturer offers so the select lists the real lineup with model names).
- **debian/changelog generated from CHANGELOG.md** (packaging): the tracked `debian/changelog` (stale at 1.0.0) is removed from git and becomes a build artifact, so release history is maintained in exactly one place — same design as the OpenAstro Guider. New `scripts/changelog_to_deb.py` converts every released `## [X.Y.Z] - date` section into a proper Debian stanza (categorized bullets, markdown stripped, release date preserved at noon local) and synthesizes an `UNRELEASED` top stanza from this repo's `## [X.Y.Z] - UNRELEASED` section when the `VERSION` file points at an uncut release (warning on label/VERSION mismatch). New `scripts/build_deb.sh` is the package build entry point: generates the changelog, validates it with `dpkg-parsechangelog --all`, then runs `dpkg-buildpackage -us -uc -b`. Generator verified against the full 28-release history (all stanzas parse) plus both unreleased paths; shellcheck clean.
- **Web UI restyled to the OpenAstro dark astro theme** (AlpacaHTTP): `web/style.css` rewritten in the OpenAstro Guider design language — shared CSS-variable palette (`#0b0e14` background, panel cards with `#232c3d` hairlines, coral `#e85d4a` accent, blue `#4a9de8` secondary), compact brand header bar replacing the old centered light-theme banner — 28px logo, accent-colored product name in natural inline flow (a `.title-text` wrapper keeps the flex `gap` from inflating the word space), and a guider-pill version badge (`v1.0.3`, dim monospace, populated at load from the management API's `ManufacturerVersion`, i.e. the `VERSION` file value, never hardcoded). The bar's vertical padding is tuned so it renders at the same ~57px height as the OpenAstro Guider's header, and the tagline ("Building the Future of Astrophotography & EAA") is removed to match the guider's chrome, underline-style tabs, pill-shaped Details toggles, dark form controls with blue focus rings, and monospace value readouts. Every existing class name is preserved, so `web/index.html` only changes its header markup and `web/app.js` is untouched. Verified in headless Chromium against a live server: Devices (collapsed + expanded card), Configure (vendor form), and Server Info (sections, logging, danger buttons) all render in the new theme.
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
- **Exception-safe disconnect in ref-counted SDK wrappers and their drivers** (AlpacaCore): the Player One PW and ZWO EFW/EAF/CAA wrappers decremented the per-handle open count and then called the SDK close *before* erasing the bookkeeping entry — a throwing close (e.g. device unplugged mid-session) left a zero-count entry behind, turning every later `close_*` into a silent no-op and leaking the SDK handle until process exit. The wrappers now erase the entry first, then surface the close error. Likewise, the matching drivers (Player One filterwheel, ZWO filterwheel/focuser/rotator) cleared `connected_`/handle/cached-info *after* the SDK close in `set_connected(false)`, so a throwing close trapped the driver half-connected (`Connected` stuck true); they now clear driver state before attempting the close, so the error still surfaces but the driver always ends up disconnected. Found by PR review on the Phoenix Wheel driver; fixed across all occurrences of the shared template.
- **GPIO power-switch driver hardening (StellaVita, iMate PowerBox, ASIAIR Pro / Plus Pi CM4, ASIAIR Plus RK3568, ZWO dew heater)** (AlpacaCore + AlpacaHTTP): a sweep of robustness fixes applied consistently across every Switch driver. (1) `SetSwitchValue` now rejects a non-finite (`NaN`/`Inf`) value from the HTTP API with `InvalidValue` before it reaches `std::lround`, which is undefined behaviour on non-finite input — fixed in the StellaVita, iMate, ASIAIR Pro/Plus, RK3568, and ZWO dew-heater drivers. (2) The libgpiod soft-PWM wrappers (StellaVita, iMate, ASIAIR) now serialise **every** `gpiod_line_request_set_value` call (boolean writes, per-port PWM worker writes, and close-time settle writes) through a dedicated `io_mutex_` held only around the ioctl — closing a thread-safety gap where a mixed boolean+PWM config issued concurrent writes on the same `gpiod_line_request` from a worker thread and a client thread (libgpiod v2 does not document the request as safe for concurrent writers). The RK3568 wrapper already serialised all ioctls through its existing mutex and was unaffected. (3) The wrappers now validate that the configured `gpioChip` is an absolute `/dev/` device node at construction (rejecting `""`, a relative path, or a bare `gpiochip0` with `InvalidValue`) instead of surfacing an opaque `gpiod_chip_open` failure later. (4) The close-time PWM settle writes now check their return value and `WARN`-log a failure instead of releasing the line in a silently-indeterminate state. (5) The web UI now always persists the per-port `ports` overlay and `pwmFrequencyHz` for the StellaVita and iMate switches, so a custom PWM frequency or per-port config survives a re-save with every PWM box un-ticked. (6) The read/write members (`GetSwitch`, `GetSwitchValue`, `GetStateChangeComplete`, etc.) now validate the switch ID **before** the connection check across all five drivers, so an out-of-range ID throws `InvalidValue` regardless of connection state per the ASCOM Switch spec (previously a disconnected `GetSwitch(-1)` returned `NotConnected`). Added unit tests for the GPIO-chip-path validation and the ID-before-connection ordering on all the switch drivers.
- **Alpaca `Value` no longer guessed by re-parsing strings** (AlpacaHTTP): `to_json(AlpacaResponse)` used to take any string `Value` and try `nlohmann::json::parse()` on it, substituting the parsed result if it succeeded. That corrupted legitimate string-typed ASCOM properties whose text happens to be valid JSON — a serial/sensor name like `"12345"` became the number `12345`, `"true"` became a boolean, etc. (wrong type on the wire). It also forced structured endpoints (`configureddevices`, `description`, `apiversions`, log payloads) to `.dump()` an array/object into a string purely so `to_json` would parse it back. `AlpacaResponse::value` is now `std::optional<nlohmann::json>` (was `optional<variant<bool,int32,double,string>>`), so handlers assign scalars, strings, arrays, and objects directly and `to_json` emits them verbatim — strings stay strings, structured values stay structured. All sixteen handlers that previously `.dump()`-ed a payload into a string drop the `.dump()`: the seven management handlers (`description`, `configureddevices`, `apiversions`, configuredevice info, `loglevel`, logfiles ×2) and the nine device-API array handlers (`SupportedActions`, `DeviceState`, `AxisRates` ×2, camera `Gains`/`Offsets`/`ReadoutModes`, filter `Names`/`FocusOffsets`) — the latter were caught by ConformU, which rejected the stringified arrays with "The JSON value could not be converted to IList`<String>`". New `test_json` cases lock in that a JSON-looking string `Value` stays a string and structured values round-trip as JSON; a `test_routing` case asserts `SupportedActions` serializes as a JSON array.
- **ASIAIR Plus (Pi CM4) switch reported itself as a "Pro"** (AlpacaCore + AlpacaHTTP): the Pi CM4 ASIAIR Plus shares the Pi 4 ASIAIR Pro's on-board GPIO wiring and reuses the same libgpiod switch driver, but the driver hard-coded "ASIAIR Pro" in `get_name()`/`get_description()`/`get_driver_info()`, so a CM4 Plus identified as a Pro in ConformU and to clients. `AsiairSwitchConfig` now carries a `model_name` (default `"ASIAIR Pro"`) that the three strings interpolate, and the router sets it to `"ASIAIR Plus (Pi CM4)"` for `switchType: asiair-plus-picm4`. Added a unit test covering the Plus label. (The committed CM4 Plus ConformU log still shows the old "Pro" strings until re-run on hardware.)
- **AlpacaHTTP hand-rolled tests silently did nothing under `-DNDEBUG`** (AlpacaHTTP/tests): `test_routing`, `test_json`, `test_config`, and `test_discovery` drove their checks — including side-effecting calls like `request.parse(...)` — through `assert()`. In a Release/`NDEBUG` build (which is what `debian/rules` and the released `.deb` use) `assert()` is stripped, so those calls never ran and the tests validated nothing (and `test_routing` then crashed on the resulting empty state). Replaced `assert()` with a new always-on `EXPECT()` macro (`tests/test_assert.h`) that evaluates its expression exactly once and aborts on failure regardless of build type. Full AlpacaHTTP suite now passes in a Release build (154/154). Server code was never affected — the bug was entirely in the tests.
- **Static-analysis cleanup of pre-existing cppcheck/clang-tidy findings** (AlpacaCore + AlpacaHTTP): cleared the findings the diff-scoped CI gates surface in files this work touches (issue #64). The one real bug: the iOptron telescope destructor called `set_connected(false)` directly, so a throw during teardown would propagate out of the implicitly-`noexcept` destructor and call `std::terminate()` — now wrapped in a best-effort `try/catch` whose handler logs (an empty handler trips `bugprone-empty-catch`). Both the iOptron and Celestron destructors now call `set_connected` with explicit class qualification so it binds statically — this is the intended behavior in a destructor and clears `virtualCallInConstructor` (cppcheck) and `clang-analyzer-optin.cplusplus.VirtualCall`. The four `identicalInnerCondition` warnings (Celestron + iOptron telescope drivers) are false positives — the inner `if (!site_info_valid_)` re-check detects a *failed* `ensure_site_info_cached_locked()` populate, which cppcheck can't see through the const call — and were silenced with documented `// cppcheck-suppress` comments. The router's `is_expected_not_implemented` `||` chain dropped its `PropertyNotImplemented`/`MethodNotImplemented` arms (both alias the same 0x400 code as `NotImplemented`; cppcheck flagged the redundancy as `knownConditionTrueFalse`). Remaining mechanical perf/style fixes, no behavior change: `x = x.substr(0,n)` → `x.resize(n)` (Celestron/SynScan protocol wrappers, HTTP router), old-style `(struct sockaddr*)` casts → `reinterpret_cast` (discovery + HTTP server), `strip_status_prefix` takes `const std::string&`, a redundant `.c_str()` dropped, and `find("web/") != 0` → `!starts_with("web/")`. **Deferred:** the ZWO ASIair `returnByReference` findings (`device_path()`/`gpio_chip_path()` → `const std::string&`) are left for a follow-up — those files `#include <gpiod.h>`/`<pwm_gpio.h>`, and the clang-tidy gate configures with `ALPACACORE_ENABLE_ALL_VENDORS=OFF`, so any edit to them fails the gate on unresolved vendor headers (libgpiod v1 in CI vs the v2 API in-tree). The ZWO mount `useInitializationList` fix landed (that file pulls in no SDK headers).
- **Server startup self-deadlock when preferred log directory is unwritable** (AlpacaHTTP): `configure_log_directory` in `util/logging_adapter.cpp` was emitting the `"Log directory '...' not writable; using fallback '...'"` (and the "No writable log directory available; file logging disabled") warnings *while still holding* `g_file_mutex`. The warning routes through the registered log sink → `write_to_file`, which re-acquires the same non-recursive `std::mutex` on the same thread — instant self-deadlock, manifesting as the server hanging single-threaded on `futex_wait` immediately after the WARN line is printed. Any fresh install where `/var/log/AlpacaBridge` does not exist or is not writable hit this (it's the exact scenario `build_and_run.sh` documents with its `"Note: skipping /var/log/AlpacaBridge setup (sudo unavailable)"` log line — no `.deb`, no passwordless sudo, no log dir, no listener). Fix: capture the warning text into a local string under the lock, release the lock at scope exit, then emit `log_warning` from the now-lock-free section. Verified by running with `ALPACAHTTP_LOG_DIRECTORY` pointed at a non-existent `/var/log/*` path — server now binds within 1 second and serves `/management/v1/configureddevices` correctly.
- **Alpaca Discovery** (AlpacaHTTP): UDP discovery listener on port 32227 no longer tears itself down when the multicast group join fails. The ASCOM Alpaca discovery protocol primarily uses UDP broadcast to `255.255.255.255:32227` (which a socket bound to `INADDR_ANY:32227` already receives), so multicast-join failure is now logged as a warning and discovery continues serving broadcast/unicast probes. Fixes NINA "Discover Servers" returning zero results when AlpacaBridge runs on an RPi acting as its own Wi-Fi access point (NetworkManager `ipv4.method shared`), where `wlan0` has no default multicast route and `IP_ADD_MEMBERSHIP` fails. Externally-routed LAN setups are unaffected.
- **Alpaca parameter names now case-insensitive for every client** (AlpacaHTTP): the ASCOM Alpaca API definition states "Parameter names are not case sensitive, so clients and drivers should be prepared for parameter names to be supplied … with any casing." The server previously enforced case-sensitive matching *only* when the `User-Agent` was ConformU and accepted any casing otherwise — so conformance-test behavior differed from production — and PUT form-body parameter names were effectively case-sensitive for every client. Query and form parameter lookups are now consistently case-insensitive and the `User-Agent`-gated strict path was deleted, so test behavior equals production behavior. Added vendor-free regression tests.
- **HTTP 400 for requests the device cannot interpret** (AlpacaHTTP): per the Alpaca spec ("HTTP 400 indicates that the device could not interpret the request e.g. an invalid device number or misspelt device type"), an unknown device type, unknown method, or unregistered device number now returns HTTP **400** instead of 404. Genuinely unroutable URLs still return 404, and driver exceptions still return HTTP 200 with `ErrorNumber` (unchanged). Added regression tests for the three 400 cases.

</details>

<details>
<summary><strong>[1.0.3] - 2026-05-07</strong></summary>

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

</details>

<details>
<summary><strong>[1.0.2] - 2026-04-18</strong></summary>

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

</details>

<details>
<summary><strong>[1.0.1] - 2026-03-27</strong></summary>

### Added
- SynScan Telescope: Sky-Watcher HEQ5 PRO ConformU validation (x64 and ARM64).

### Changed
- SynScan Telescope: Device name changed from "SynScan Mount"/"SynScan Telescope" to "SynScan V3/V4 Telescope" across the driver and web portal to clarify hand controller compatibility.
- SynScan `SideOfPier` mapping: swapped `pierEast`/`pierWest` values to match ASCOM convention.
- SUPPORTED-DRIVERS.md: Added Sky-Watcher HEQ5 PRO to SynScan mount table.

</details>

<details>
<summary><strong>[1.0.0] - 2026-03-26</strong></summary>

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

</details>

<details>
<summary><strong>[0.13.0] - 2026-03-12</strong></summary>

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

</details>

<details>
<summary><strong>[0.12.1] - 2026-02-25</strong></summary>

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

</details>

<details>
<summary><strong>[0.12.0] - 2026-02-21</strong></summary>

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

</details>

<details>
<summary><strong>[0.11.2] - 2026-02-20</strong></summary>

### Fixed
- **Discovery service** (AlpacaHTTP)
  - Discovery loop now uses `select()` with a 200 ms timeout so `stop()` can terminate promptly on all platforms instead of blocking on `recvfrom()`. Removed socket close from `stop()` so the discovery thread exits cleanly; added error handling for `select()` and `recvfrom()` (interrupted / would-block continue; other errors logged and break).

</details>

<details>
<summary><strong>[0.11.1] - 2026-02-19</strong></summary>

### Changed
- **AlpacaCore tests** (AlpacaCore)
  - Tests now require Catch2 only (doctest fallback removed). CMake supports both `Catch2::Catch2WithMain` and `Catch2::Catch2Main` for Catch2 v2/v3 compatibility.
  - SynScan tests use `catch2_compat.h` for Catch2 include compatibility.

</details>

<details>
<summary><strong>[0.11.0] - 2026-02-19</strong></summary>

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

</details>

<details>
<summary><strong>[0.10.0] - 2026-02-03</strong></summary>

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

</details>

<details>
<summary><strong>[0.9.0] - 2026-02-03</strong></summary>

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

</details>

<details>
<summary><strong>[0.8.6] - 2026-01-20</strong></summary>

### Changed
- **HTTP Request Handling** (AlpacaHTTP)
  - Merge multiple `Accept` headers into a single comma-delimited value.
- **ImageBytes Mapping** (AlpacaHTTP)
  - Standardized image-bytes element type codes for 64-bit variants and accepted `long`/`ulong` aliases.
  - Added visibility logging when `imagearray`/`imagearrayvariant` evaluate image-bytes negotiation.

</details>

<details>
<summary><strong>[0.8.5] - 2026-01-19</strong></summary>

### Changed
- **iOptron Network Reliability** (AlpacaCore)
  - Added socket send/receive timeouts for TCP connections.
  - Allow partial responses when a terminator is not required to avoid unnecessary timeouts.
  - Treat missing :MS1/:MS2 replies over network as accepted slews with a warning, matching WiFi bridge behavior.

</details>

<details>
<summary><strong>[0.8.4] - 2026-01-17</strong></summary>

### Added
- **ImageBytes Streaming** (AlpacaHTTP)
  - Added `application/imagebytes` handling so `camera.imagearray` can stream compact binary payloads instead of JSON when clients request it.
  - Honored `camera.imagearrayvariant` metadata, inferred transmission element widths, and included numeric metadata plus transaction IDs alongside the pixel data.
  - Streamed structured error payloads with the same metadata layout so Alpaca exceptions can still be parsed when image bytes responses fail.

</details>

<details>
<summary><strong>[0.8.3] - 2026-01-13</strong></summary>

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

</details>

<details>
<summary><strong>[0.8.2] - 2026-01-12</strong></summary>

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

</details>

<details>
<summary><strong>[0.8.1] - 2026-01-11</strong></summary>

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

</details>

<details>
<summary><strong>[0.8.0] - 2026-01-10</strong></summary>

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

</details>

<details>
<summary><strong>[0.7.0] - 2026-01-09</strong></summary>

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

</details>

<details>
<summary><strong>[0.6.1] - 2026-01-08</strong></summary>

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

</details>

<details>
<summary><strong>[0.6.0] - 2026-01-06</strong></summary>

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

</details>

<details>
<summary><strong>[0.5.0] - 2026-01-05</strong></summary>

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

</details>

<details>
<summary><strong>[0.4.0] - 2026-01-03</strong></summary>

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

</details>

<details>
<summary><strong>[0.3.0] - 2025-12-16</strong></summary>

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

</details>

<details>
<summary><strong>[0.2.1] - 2025-12-04</strong></summary>

### Changed
- **License Headers** (AlpacaHTTP 0.2.1)
  - Updated all source files with new license header format
  - Changed license URL to GitHub repository location
  - Added SSPL v1 compliance notice to all headers

</details>

<details>
<summary><strong>[0.2.0] - 2025-12-02</strong></summary>

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

</details>

<details>
<summary><strong>[0.1.0] - 2025-12-02</strong></summary>

### Added
- **Initial Release**
  - AlpacaCore: Core Alpaca protocol library with device driver interfaces
  - AlpacaHTTP: HTTP/1.1 server with Alpaca API routing
  - Complete directory structure following architecture guidelines
  - CMake build system with C++20 support
  - Test infrastructure with Catch2 support
  - Example servers and device implementations
  - Comprehensive documentation

</details>
