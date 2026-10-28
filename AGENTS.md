# Agent Instructions

This file owns the core agent rules for this repository. Shared rules for one
area (driver lifecycle, ASCOM contract, tests, CI, packaging) live in topic files
under `.github/instructions/`, listed in the
[instruction index](docs/agent-instructions.md); vendor deltas live beside them.

Resolved incident records live in `docs/failures/`; design rationale lives in
`docs/decisions/`. Keep current rules here and in scoped instruction files, with
short pointers to those records rather than copying their history into new rules.

Domain terms that are easy to confuse (device number vs enumeration index, API vs
persisted config, cancelled vs superseded, Connected vs link health, fake vs rig
evidence) are defined in [CONTEXT.md](CONTEXT.md); module names are in
[the architecture overview](docs/architecture.md#modules).

Issues live in GitHub Issues on `open-astro/AlpacaBridge`, via the `gh` CLI —
see `docs/agents/issue-tracker.md` for conventions, including the `P1`-`P5`
priority label every open issue carries.

## Load the complete instructions before working

Read this entire file before planning, reviewing, or editing. Then read
[the instruction index](docs/agent-instructions.md) and, in full, every shared
topic file and vendor file relevant to the task, including vendors mentioned in
the request or affected through shared routing, configuration, SDKs, or tests.
Read long files in bounded chunks until the end; a truncated tool result does not
count as a complete read. Follow relevant cross-references and memory records.
Revisit the index when the task expands. These are required reads even when the
client does not interpret `applyTo`.

## Where shared rules live

| Topic file (`.github/instructions/`) | Read before |
| --- | --- |
| [`driver-lifecycle`](.github/instructions/driver-lifecycle.instructions.md) | writing or changing any driver, wrapper, thread, connect/disconnect path or cache |
| [`ascom-contract`](.github/instructions/ascom-contract.instructions.md) | choosing an error code, DeviceState, telescope/camera/filter wheel/switch semantics |
| [`serial-autodetect`](.github/instructions/serial-autodetect.instructions.md) | serial or socket I/O, port scans, auto-detect factories |
| [`testing`](.github/instructions/testing.instructions.md) | adding tests, fakes, SDK seams, ConformU runs |
| [`ci-gates`](.github/instructions/ci-gates.instructions.md) | touching CI, `scripts/`, pre-flight, the stress harness |
| [`packaging-release`](.github/instructions/packaging-release.instructions.md) | CMake vendor wiring, SDK `.so`, `.deb`, changelog fragments, releases, root helpers |
| [`http-integration`](.github/instructions/http-integration.instructions.md) | adding a vendor or device type to AlpacaHTTP, the web UI, logging |

A rule that applies to a second vendor goes in the matching topic file, never in
a vendor file. This file stays under 15,000 bytes (`scripts/check_docs_drift.py`
check 21): add a one-line pointer here, not the rule.

## Rules that apply to every change

Each line is a summary; the linked topic file holds the rule and its history.

- Inherit `AsyncConnectable` for connect/disconnect and run operation bodies in a
  `util::AsyncOperation` slot; never hand-roll either
  ([driver-lifecycle](.github/instructions/driver-lifecycle.instructions.md)).
- An async operation body that fails keeps its error, and the device's completion property
  (IsMoving, Position, CoverState/CalibratorState, ImageReady, Slewing, ...) raises it until the
  next command or a reconnect clears it; never log-and-report-done (the ASCOM async FAQ)
  ([ascom-contract](.github/instructions/ascom-contract.instructions.md#a-failed-async-operation-body-keeps-its-error-all-drivers)).
- Never `.detach()` a thread that touches `this`; join every thread before members die.
- Getters never wait on device I/O: `mutex_` is a short state lock, never held across
  an SDK call, serial exchange or connect handshake, except a camera's short
  `with_camera`/`with_handle` SDK calls (kept until a stall is measured); one per-link
  I/O lock serialises the wire. Lock order in a converted driver: `transition_mutex_` →
  I/O lock → `mutex_` (leaf of the driver's own locks) → SDK-wrapper mutex, with the
  `StateSnapshot`, slot and `CaptureSession` mutexes as leaves under `mutex_`. Cameras and unconverted
  drivers keep `mutex_` → operation lock → SDK-wrapper mutex
  ([decision 0016](docs/decisions/0016-getters-never-wait-on-device-io.md)).
- `set_connected(false)` clears driver state before the SDK close; a connect guards all
  post-open init so a throw closes the handle.
- `InvalidValue` precedes the connection check; every other member throws
  `NotConnected` while disconnected
  ([ascom-contract](.github/instructions/ascom-contract.instructions.md#ascom-exception-vocabulary-pick-the-right-one--conformu-checks-it)).
- Declare every persisted config field in the vendor's catalog schema; prefix every
  non-ZWO form field name
  ([http-integration](.github/instructions/http-integration.instructions.md#enumeration-index-fields--unique-names--auto-numbering-all-vendors)).
- Fixing one review finding: fix every sibling site in the same commit and name them
  ([driver-lifecycle](.github/instructions/driver-lifecycle.instructions.md#when-fixing-a-review-finding-avoid-the-regression-treadmill)).
- Every new driver ships the 8+1 required test cases (cases 3 and 4, and part of case 6, come from its `contract_sweep.h` entry) and a `[stress]` registration;
  every new test case is listed under `## Falsified by` in the PR body
  ([testing](.github/instructions/testing.instructions.md)).
- Every PR adds a `changelog.d/<branch-slug>.md` fragment and never edits
  `CHANGELOG.md` or `debian/changelog`
  ([packaging-release](.github/instructions/packaging-release.instructions.md#changelog-fragments-one-file-per-pr)).
- Use the AlpacaCore logging sink, never ad-hoc stdout/stderr, and `AlpacaException`
  for error paths.
- A change must agree with the Rules of every decision record it falls under; a change
  to a record needs a collaborator's authorization ([decisions](docs/decisions/README.md)).

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

Supported device types (base drivers in `AlpacaCore/src/drivers/`): Camera, Telescope, FilterWheel, Focuser, Rotator, Dome, Switch, CoverCalibrator, ObservingConditions, SafetyMonitor. These are exactly the 10 ASCOM Alpaca device types — do not invent new top-level device types.

## Language, Style, and Safety

- C++20 preferred, RAII, small focused functions.
- Use `#pragma once` in headers.
- Prefer `enum class`, `std::chrono`, `std::string_view` where appropriate.
- No `using namespace std;` in headers.
- Core/driver layers should avoid heavy framework dependencies.
- License headers must remain AGPL-3.0-or-later and unmodified in all source files.

## Units and Behavior Conventions

- Exposure: seconds
- Angles: degrees
- RA: hours
- Dec: degrees
- Pixel size: microns
- Time: UTC with `std::chrono`

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
  - Clean thread/task shutdown in destructors — **[Driver concurrency &
    lifecycle](.github/instructions/driver-lifecycle.instructions.md#driver-concurrency--lifecycle-read-before-writing-a-driver) is the
    single most important section of the shared rules; every rule there was learned
    from a review round.**
- Add TODO comments where vendor protocol/SDK behavior is uncertain.

### ConformU telescope runs: bare mount only (SAFETY)

Never run the ConformU telescope suite with an OTA mounted. ConformU 4.5 commands 40+
maximum-rate slews (extended rate-offset tests at HA +/-9 and +/-3, MoveAxis at the full
AxisRates maximum, extended pulse-guide slews, SideOfPier model tests alternating across the
meridian), deliberate mid-slew aborts, and targets placed halfway to the horizon; recorded
mid-slew arcs on a Wave 100i dipped to ~5 degrees altitude. A mounted scope risks pier
strikes, cable snags, and balance failures; strain-wave mounts have no clutch to slip.
Validate on a bare mount, always. (The large swings during a run are ConformU's designed
choreography, not a driver fault — verify by checking that every slew lands on target.)

## Vendor-Specific Notes

Vendor notes contain **vendor specifics and deltas only** — general rules (concurrency,
ROI alignment, GPIO/soft-PWM, FilterWheel semantics, config round-trip) live in the
shared topic files above. If a rule would apply to a second vendor, it belongs there, not here.

Vendor-specific guidance lives in `.github/instructions/<vendor>.instructions.md`, scoped to each vendor’s implementation and tests. Rules shared by multiple vendors belong in the shared topic files; do not duplicate them in vendor instructions. AlpacaHTTP protocol guidance lives in `.github/instructions/alpaca-http-conformance.instructions.md`.

## General Notes

Historical evidence: [July 2026 code audit](docs/failures/2026-07-11-code-audit.md). Read it as a historical snapshot, not current unresolved findings.

- On Linux, ensure udev rules in `AlpacaCore/external/**/*.rules` are installed. Some vendor SDKs (e.g. QHY) ship multiple copies of the same rules file under different subdirectories — deduplicate by basename when installing so only one copy lands in `/etc/udev/rules.d/`. Keep `build_and_run.sh` and `install_alpaca_service.sh` in sync; both contain the udev/firmware install logic.
- ConformU logs live under `AlpacaCore/conformu/`.

## Out of Scope Guardrails

- Do not add HTTP/server code to AlpacaCore.
- Do not add vendor SDK usage to AlpacaHTTP.
- Do not add desktop GUI frameworks (Qt, GTK, wxWidgets, etc.). The web UI in `AlpacaHTTP/web/` is the only user interface.
- Do not invent device types outside the ASCOM Alpaca standard set (Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope). Shutter control is part of the **Dome** interface (`OpenShutter`/`CloseShutter`/`ShutterStatus`), not a standalone device. A non-standard `Shutter` device type existed as unused scaffolding and was removed 2026-06-09 — clients (NINA, ConformU) cannot consume non-standard types, so they break interoperability.
