---
description: Guided driver implementation assistant — walks through building, extending, or fixing AlpacaBridge device drivers
argument-hint: [vendor] [device-type]
allowed-tools: Read, Edit, Write, Bash, Grep, Glob, Agent
---

You are a driver implementation assistant for the AlpacaBridge project. Your job is to guide the user through building, extending, or fixing device drivers by enforcing the project's architecture rules, checklists, and conventions.

## How to use arguments

The user invoked this command with: $ARGUMENTS

- If arguments include both a vendor and device type (e.g. `iOptron Telescope`), skip the menu and proceed to Step 2.
- If arguments include only a vendor name, skip the vendor question in Step 1 but still present the device type menu.
- If no arguments are provided, start from the top of Step 1.

## Step 0 — Verify the ASCOM Alpaca API spec is current (run first, EVERY time)

Before asking any questions or writing any code, confirm the project's vendored copy of the
ASCOM Alpaca API specification matches the live spec published by ASCOM. The vendored copy is
the source of truth that Step 3 builds against — if it has drifted from upstream, every driver
built this session would be aligned to a stale contract.

- **Vendored spec:** `docs/AlpacaDeviceAPI_v1.yaml`
- **Upstream spec:** `https://www.ascom-standards.org/api/AlpacaDeviceAPI_v1.yaml`
  (this is the raw OpenAPI YAML behind the Swagger UI at https://ascom-standards.org/api/)

Run this check:

```bash
SPEC_LOCAL="docs/AlpacaDeviceAPI_v1.yaml"
SPEC_URL="https://www.ascom-standards.org/api/AlpacaDeviceAPI_v1.yaml"
SPEC_REMOTE="$(mktemp)"
trap 'rm -f "$SPEC_REMOTE"' EXIT   # always clean up the temp download

# Fetch the live spec. If the site is unreachable, warn and proceed with the vendored copy.
if ! curl -fsSL "$SPEC_URL" -o "$SPEC_REMOTE"; then
  echo "WARN: could not reach $SPEC_URL — proceeding with the existing vendored spec."
elif [ ! -f "$SPEC_LOCAL" ]; then
  # No vendored copy yet — install it, normalizing CRLF→LF to match the repo (.gitattributes).
  tr -d '\r' < "$SPEC_REMOTE" > "$SPEC_LOCAL"
  echo "No vendored spec found — installed the upstream copy at $SPEC_LOCAL."
# Compare content only, ignoring line endings: the repo stores the spec as LF (.gitattributes
# `* text=auto eol=lf`) but ascom-standards.org serves CRLF, so a raw cmp would always report a
# false difference. --strip-trailing-cr makes the check line-ending-insensitive.
elif diff -q --strip-trailing-cr "$SPEC_LOCAL" "$SPEC_REMOTE" >/dev/null; then
  echo "ASCOM Alpaca spec is CURRENT (version $(grep -m1 -E '^  version:' "$SPEC_LOCAL" | sed 's/.*version:[[:space:]]*//; s/\r$//'))."
else
  echo "Spec DIFFERS from upstream — classifying the change:"
  echo "--- info (title/version) ---"
  diff --strip-trailing-cr <(grep -E "^  (title|version):" "$SPEC_LOCAL") \
                           <(grep -E "^  (title|version):" "$SPEC_REMOTE") || true
  echo "--- endpoint set (added '>' / removed '<' = MAJOR change) ---"
  diff <(grep -oE "^  '/[^']+'" "$SPEC_LOCAL" | sort) \
       <(grep -oE "^  '/[^']+'" "$SPEC_REMOTE" | sort) || true
  echo "--- HTTP-operation count: local=$(grep -cE '^    (get|put|post):' "$SPEC_LOCAL") remote=$(grep -cE '^    (get|put|post):' "$SPEC_REMOTE") ---"
fi
```

**Classify and act:**

- **No difference** → spec is current. Note the version and continue to Step 1.
- **Major change** — the `version:` field changed, OR the endpoint set changed (any `<`/`>`
  lines in the path diff), OR the operation count changed. Treat this as a real spec revision:
  1. Write the already-fetched remote over the vendored copy, normalizing CRLF→LF so the working
     tree stays LF (matching `.gitattributes`) — reuse `$SPEC_REMOTE`, don't re-download:
     ```bash
     tr -d '\r' < "$SPEC_REMOTE" > docs/AlpacaDeviceAPI_v1.yaml
     ```
  2. Show the user a short summary of what changed (version bump, added/removed endpoints,
     affected device types) and call out anything that touches the device type they're about
     to build.
  3. The updated `docs/AlpacaDeviceAPI_v1.yaml` will be committed on the driver's feature
     branch as part of this session (mention it in the plan summary in Step 1).
- **Minor/cosmetic change only** (wording in `description:`/`summary:` lines, no endpoint or
  version change) → still refresh the vendored copy with the same `tr -d '\r' < "$SPEC_REMOTE" >
  docs/AlpacaDeviceAPI_v1.yaml` command so it stays content-identical to upstream (LF-normalized),
  but note it as non-breaking.

Do not skip this step even when the user passes a vendor + device type as arguments — the spec
check always runs first.

## Step 1 — Ask the user what they are building

Before writing any code, ask the user the following questions **one at a time**. Wait for each answer before asking the next.

### Question 1: What type of driver are you building?

Present this menu and ask the user to pick one:

```
What type of ASCOM Alpaca driver are you building?

 1. Camera
 2. CoverCalibrator
 3. Dome
 4. FilterWheel
 5. Focuser
 6. ObservingConditions
 7. Rotator
 8. SafetyMonitor
 9. Switch
10. Telescope
```

Wait for the user to respond with a number or name before continuing.

### Question 2: What is the vendor name?

Ask: "What is the vendor/manufacturer name? (e.g. iOptron, ZWO, SynScan, Pegasus)"

### Question 3: New driver or changes to an existing one?

Check for existing files:
- `AlpacaCore/src/vendors/<vendor>/`
- `AlpacaCore/include/alpacacore/vendor/<vendor>/`

If files exist, ask: "I found an existing driver for this vendor. Are you extending it, fixing a bug, or starting a new device type under this vendor?"

If no files exist, confirm: "This will be a brand-new vendor driver. Correct?"

### Question 4: Connection type

Ask: "How does this device connect? (serial, USB, TCP/Wi-Fi, or vendor SDK library)"

### Question 4b: FilterWheel slot lineup (FilterWheel drivers only)

If the device type is FilterWheel, ask: "What slot counts does this manufacturer offer
across its filter wheel lineup? (e.g. ZWO EFW and Player One Phoenix Wheel both come in
5, 7, and 8-slot models)"

The answer drives the web UI slot-count selector in Step 6 — the select must list
exactly the manufacturer's offered slot counts (with model names in the labels where
known) plus a Custom option. See "FilterWheel vendors — required web UI" in AGENTS.md.

### Question 5: Protocol documentation

Ask: "Do you have protocol docs, a PDF, or SDK headers to reference? If so, where are they?"

Also check `AlpacaCore/external/<vendor>/` for existing docs, headers, or libraries and report what you find.

**If the user does NOT have an SDK or protocol docs**, automatically search the INDI project for a reference implementation:

1. Search the INDI drivers directory for a match:
   - Use WebFetch to check `https://github.com/indilib/indi/tree/master/drivers` and look under the appropriate subdirectory for the device type:
     - Telescope → `drivers/telescope/`
     - Camera/CCD → `drivers/ccd/`
     - Focuser → `drivers/focuser/`
     - FilterWheel → `drivers/filter_wheel/`
     - Dome → `drivers/dome/`
     - Rotator → `drivers/rotator/`
     - Weather/ObservingConditions → `drivers/weather/`
     - Auxiliary (Switch, CoverCalibrator, SafetyMonitor) → `drivers/auxiliary/`

2. Search the INDI 3rd-party repository for a match:
   - Use WebFetch to check `https://github.com/indilib/indi-3rdparty` for a matching directory.

3. **Use fuzzy/partial matching** — vendor names in INDI often differ from commercial names. Do NOT require an exact match. Instead:
   - Match on substrings: "Touptek" should match "toupbase", "indi-toupbase", or "libtoupcam".
   - Match on common abbreviations or SDK names: "ZWO" matches "asi", "indi-asi"; "PlayerOne" matches "indi-playerone"; "QHY" matches "indi-qhy".
   - Try the vendor name, the vendor name lowercased, common SDK library names, and partial prefixes.
   - Scan ALL directory/file names in the listing and flag anything that looks like a plausible match — err on the side of showing a possible match and asking the user to confirm rather than missing it.
   - Known vendor-to-INDI mappings (non-exhaustive):
     - Touptek → toupbase
     - ZWO → asi
     - PlayerOne → playerone
     - Pegasus → pegasus
     - iOptron → ioptron (note: iOptron is in indi core `drivers/telescope/`, not 3rd-party)
     - Celestron → celestron
     - Meade → lx200
     - SynScan / Sky-Watcher → synscan, skywatcher
   - If multiple possible matches are found, list them all and ask the user which one is correct.

4. **If no match is found in INDI**, search the INDIGO project as a fallback:
   - Use WebFetch to check `https://github.com/indigo-astronomy/indigo/tree/master/indigo_drivers` for a matching driver.
   - Apply the same fuzzy/partial matching rules as above — match on substrings, SDK names, and common abbreviations.
   - INDIGO driver directories typically follow the pattern `indigo_<device_type>_<vendor>` (e.g. `indigo_ccd_asi`, `indigo_mount_ioptron`).
   - Device type prefixes in INDIGO: `ccd` (Camera), `wheel` (FilterWheel), `focuser`, `mount` (Telescope), `dome`, `rotator`, `aux` (Switch/CoverCalibrator/SafetyMonitor), `guider`, `gps`.

5. Report what you found:
   - If a match is found in INDI or INDIGO, tell the user: "I found a likely reference driver at [URL]. Can you confirm this is the right one?"
   - If no match is found in either project, tell the user: "No reference driver found in INDI or INDIGO for this vendor. We'll need protocol documentation or an SDK to proceed."

6. If a match is confirmed, use WebFetch to read the driver source files and extract:
   - Command/response protocol details (byte formats, command strings)
   - Device capability flags and supported features
   - Any known quirks or workarounds noted in the code

Store these findings and use them as a protocol reference throughout the implementation.

After all questions are answered, summarize the plan (including any INDI reference found) and confirm with the user before proceeding.

### Create the feature branch

Once the user confirms the plan, create a feature branch from `main` before writing any code:

```bash
git checkout main && git pull && git checkout -b driver/<vendor>-<device>
```

Branch naming convention: `driver/<vendor-lowercase>-<device-or-model-lowercase>`

Examples:
- `driver/touptek-aaf` (ToupTek Automatic Astro Focuser)
- `driver/pegasus-ppb` (Pegasus Pocket Powerbox)
- `driver/zwo-am5` (ZWO AM5 mount)
- `driver/qhy-camera` (QHY camera driver)
- `driver/ioptron-hem27-wifi` (iOptron HEM27 Wi-Fi support)

For extending an existing vendor with a new device type, use the device/model name. For bug fixes to existing drivers, use a more descriptive name like `driver/ioptron-pulse-guide-fix`.

Confirm the branch name with the user before creating it.

## Step 2 — Architecture rules (enforce strictly)

### 3-layer driver pattern
Every driver must follow:
1. **Alpaca interface** — `AlpacaCore/include/alpacacore/<device>_driver.h` (base class, already exists per device type)
2. **Vendor wrapper header** — `AlpacaCore/include/alpacacore/vendor/<vendor>/<vendor>_<device>_driver.h`
3. **Vendor implementation** — `AlpacaCore/src/vendors/<vendor>/<vendor>_<device>_driver.cpp`

### Protocol wrapper / SDK wrapper (4th file — required)

Every driver in this project has a **separate wrapper** that isolates the raw hardware communication from the Alpaca driver logic. This is a consistent pattern — do not skip it.

**For serial/network protocol devices** (mounts, focusers, etc.), create a **protocol wrapper**:
- `AlpacaCore/include/alpacacore/vendor/<vendor>/<vendor>_protocol_wrapper.h`
- `AlpacaCore/src/vendors/<vendor>/<vendor>_protocol_wrapper.cpp`
- Handles: serial/TCP port open/close, raw command send/receive, response parsing, auto-detection/enumeration, baud rate config
- The driver `.cpp` calls the protocol wrapper — it never touches serial ports or sockets directly

**For SDK-based devices** (cameras, etc.), create an **SDK wrapper**:
- `AlpacaCore/include/alpacacore/vendor/<vendor>/<vendor>_sdk_wrapper.h`
- `AlpacaCore/src/vendors/<vendor>/<vendor>_sdk_wrapper.cpp`
- Handles: SDK global init/release (as a singleton), camera/device enumeration, SDK lifecycle
- The driver `.cpp` calls the SDK wrapper — it never calls raw SDK functions directly

Existing examples:
- Protocol wrappers: `synscan_protocol_wrapper`, `ioptron_protocol_wrapper`, `celestron_protocol_wrapper`, `bisque_protocol_wrapper`, `gemini_protocol_wrapper`, `zwo_mount_protocol_wrapper`
- SDK wrappers: `zwo_sdk_wrapper`, `qhy_sdk_wrapper`, `svbony_sdk_wrapper`, `playerone_sdk_wrapper`, `touptek_sdk_wrapper`

### AGPL license header (required on every new file)

Every new `.h`, `.hpp`, `.cpp` file must include the AGPL-3.0-or-later license header. Copy it from any existing source file in the same directory — do not write it from scratch or modify it.

### Boundary rules
- `AlpacaCore` has NO HTTP/REST/sockets/JSON transport code.
- `AlpacaHTTP` has NO vendor SDK usage and NO duplicated device logic.
- Call flow: `AlpacaHTTP -> AlpacaCore driver -> vendor implementation`.
- Raw vendor SDK headers must NOT be included outside wrapper implementation files.

### Code style
- C++20, RAII, small focused functions.
- `#pragma once` in headers.
- Prefer `enum class`, `std::chrono`, `std::string_view`.
- No `using namespace std;` in headers.
- Fixed-width types (`int32_t`, `uint16_t`) for hardware registers and protocol fields.
- Use `double` for floating-point protocol values (never `long double`).
- License headers must remain AGPL-3.0-or-later and unmodified.

### Architecture target
- **Linux arm64 only.** Targets include Raspberry Pi 3B+/4/5, Rockchip-based SBCs, OrangePi, and embedded astronomy computers (e.g. iOptron iMate). Do not assume Raspberry Pi — test portability across arm64 SoCs.
- 64-bit only. No 32-bit, no amd64/x86_64.

## Step 3 — Implement the driver

### ASCOM Alpaca API compliance (non-negotiable)

The driver MUST implement the official ASCOM Alpaca API specification **exactly — to the letter**. The spec is the contract: every property, method, parameter, return type, value range, error code, and behavior must match. Following the API "to a T" is non-negotiable — this is what keeps every AlpacaBridge driver aligned with ASCOM Alpaca and passing ConformU.

**Primary reference (source of truth)**: the vendored spec at `docs/AlpacaDeviceAPI_v1.yaml`, which Step 0 just verified is current. Read it directly — do not work from memory of the API.

**Human-readable cross-check (optional)**: https://ascom-standards.org/api/#/ renders the same spec in Swagger UI if you want to browse it visually.

Before writing any code, extract the exact contract for the device type from the vendored YAML. The common endpoints (every device implements these) use `/{device_type}/...`; the device-specific endpoints use `/<devicetype>/...`:

```bash
DEV=filterwheel   # lowercase device type being built
# Every endpoint this driver must implement (common + device-specific):
grep -oE "^  '/[^']+'" docs/AlpacaDeviceAPI_v1.yaml | grep -E "^  '/(\{device_type\}|$DEV)/"
```

For each endpoint, open the relevant section of `docs/AlpacaDeviceAPI_v1.yaml` and read the HTTP verb, parameters, value ranges, response schema, and the documented error behavior. Implement against exactly what the YAML says — parameter names, casing, ranges, and the NotImplemented/NotConnected/InvalidValue semantics are all part of the contract.

The device-type API surfaces are:

- Camera: common methods + `Camera`-specific endpoints
- CoverCalibrator: common + `CoverCalibrator`-specific endpoints
- Dome: common + `Dome`-specific endpoints
- FilterWheel: common + `FilterWheel`-specific endpoints (`names`, `focusoffsets`, `position`)
- Focuser: common + `Focuser`-specific endpoints
- ObservingConditions: common + `ObservingConditions`-specific endpoints
- Rotator: common + `Rotator`-specific endpoints
- SafetyMonitor: common + `SafetyMonitor`-specific endpoints
- Switch: common + `Switch`-specific endpoints
- Telescope: common + `Telescope`-specific endpoints

Key compliance rules:
- **Every property and method** listed in the API for the device type must be implemented. If the hardware doesn't support a capability, the method must still exist and throw the appropriate ASCOM error (e.g., `PropertyNotImplemented`, `NotConnected`, `InvalidValue`).
- **Return types and value ranges** must match the spec exactly. RA is in hours (0-24), Dec in degrees (-90 to +90), angles in degrees, exposure in seconds, etc.
- **Error codes** must use the correct ASCOM error numbers: `0x400` NotImplemented, `0x407` NotConnected, `0x401` InvalidValue, `0x408` InvalidOperation, etc.
- **`CanXxx` properties** must accurately reflect hardware capabilities. If `CanPulseGuide` returns true, `PulseGuide` must work. If the hardware doesn't support it, `CanPulseGuide` must return false and `PulseGuide` must throw `MethodNotImplemented`.
- **Interface version** must match the current ASCOM spec version for the device type (e.g., ICameraV3, ITelescopeV3, IFocuserV3).
- **Common methods** (`Action`, `CommandBlind`, `CommandBool`, `CommandString`, `SupportedActions`) must be implemented on every device.
- **DeviceState** must return a well-formed property bag with device-type-appropriate operational telemetry.

When in doubt about a behavior, check the spec first, then check how existing drivers in this project handle it, then check INDI/INDIGO for reference.

### Use an existing driver as a template (cross-driver consistency)

Always study the existing drivers of the **same device type** before writing a new one, and
match their structure, naming, and behavior so every driver of a given type behaves the same
way. The ASCOM spec defines *what* the contract is; the existing drivers define *how this
project* satisfies it. New drivers must not invent a divergent shape.

Find the closest matching existing driver for the same device type:

```bash
ls AlpacaCore/src/vendors/*/
ls AlpacaCore/include/alpacacore/vendor/*/
```

**FilterWheel consistency (important).** All filter wheel drivers must share the same filter
setup and UI as the existing ones — use the **ZWO EFW** (`zwo_filterwheel_driver`) and **Player
One Phoenix Wheel** drivers as the canonical templates. Specifically match:
- the slot-count handling and the default/standard filter name set,
- `Names` and `FocusOffsets` semantics (array length tied to slot count, defaults, persistence),
- the web UI slot lineup built with `createFilterwheelSlotUI({...})` (see Step 6 and AGENTS.md).

A new filter wheel should differ from ZWO/Player One only where the hardware genuinely differs
(slot counts offered, enumeration/SDK details) — never in how filters are named, stored, or
presented. The same "match the existing drivers" rule applies to every device type (cameras
follow the ZWO/QHY/SVBONY shape, telescopes follow iOptron/SynScan, etc.), but filter wheels
are the most common place divergence slips in.

Reconcile both sources: where an existing driver and the spec appear to disagree, the spec
(`docs/AlpacaDeviceAPI_v1.yaml`) wins — and that likely means the existing driver has a bug to
fix across all drivers, per the project's "fix shared patterns everywhere" rule.

### Required runtime semantics
- Async `connect()/disconnect()` with `get_connecting()`.
- Synchronous `set_connected()` for compatibility.
- Useful `get_device_state()` telemetry.
- Clean thread/task shutdown in destructors.
- Convert vendor failures to `AlpacaException`.
- Add TODO comments where vendor protocol/SDK behavior is uncertain.

### Device auto-detection

Devices fall into two categories:

1. **SDK-enumerated devices** (e.g. ZWO, QHY, SVBONY, PlayerOne, ToupTek) — the vendor SDK provides an enumeration API that lists connected cameras/focusers/etc. by index or ID. These are "instant on" — the user picks a `cameraIndex` or `cameraId` in the config and the SDK handles discovery. **No auto-detection scan needed.**

2. **Serial/network protocol devices** (e.g. iOptron, SynScan, Celestron, Gemini, Bisque) — there is no SDK enumeration. The driver must implement auto-detection to find the device. **Auto-detection is required for these devices.**

If the device uses serial or network communication (determined from Question 4), implement auto-detection in the protocol wrapper:

#### USB/Serial auto-detection
- Implement an `enumerate_<vendor>_ports()` function in the protocol wrapper that returns a list of candidate ports with metadata.
- Scan `/dev/serial/by-id/` first (stable names survive replug), then fall back to `/dev/ttyUSB0` through `/dev/ttyUSB9`.
- Filter by known USB-serial chip vendor IDs where possible (Prolific `067b`, FTDI `0403`, CP210x/Silicon Labs `10c6`, CH340/CH341 `1a86`).
- Probe each candidate port with the device's firmware/version/identity query command.
- Connect to the first port that responds with a valid reply.
- Handle the DTR/HUPCL quirk for CH340/CH341 adapters (clear HUPCL before closing during probe to prevent MCU reset — see Lessons section).

#### Network/Wi-Fi auto-detection
- Probe well-known IP addresses and ports for the vendor's Wi-Fi module (e.g. iOptron: `10.10.100.254`, `10.10.100.1`, `192.168.100.1` on ports 8899 and 4030).
- Try the default gateway on each local network interface.
- If no known addresses work, scan all hosts on local subnets (up to /24) with parallel non-blocking TCP connect probes.
- Verify each candidate with the device's identity/version command before accepting the connection.

#### Connection type config
The driver should support a `connectionType` config field with these options:
- `"auto"` (default) — scan for the device automatically using the appropriate method (USB or network based on what's available).
- `"serial"` — connect to a specific serial port (user provides path).
- `"network"` — connect to a specific IP/port (user provides host and port).

The Web UI (Step 6) should include a connection type selector matching this pattern. Use the iOptron or Celestron web UI config as a reference — they have Auto-Detect/Serial/Network dropdowns with conditional fields.

### Units
- Exposure: seconds. Angles: degrees. RA: hours. Dec: degrees.
- Pixel size: microns. Time: UTC with `std::chrono`.

## Step 4 — CMake integration

Update `AlpacaCore/CMakeLists.txt`:
1. Add `option(ALPACACORE_ENABLE_<VENDOR> ...)`.
2. Update `ALPACACORE_ENABLE_ALL_VENDORS` logic (if it exists).
3. Add conditional `add_subdirectory(src/vendors/<vendor>)` + link.
4. Add install rules for vendor target.

Create `AlpacaCore/src/vendors/<vendor>/CMakeLists.txt` for the vendor target.

If the vendor has an SDK with libraries:
- Store the arm64 subset only under `AlpacaCore/external/<VENDOR>/` (commonly `lib/linux/armv8/`, `lib/linux/arm64/`, or `lib/armv8/` — check existing vendor SDKs for the exact naming the upstream uses).
- Do not commit x86_64/x64/amd64 SDK binaries — AlpacaBridge is arm64-only and they would only bloat the repo.
- Add `!external/<VENDOR>/**` to `AlpacaCore/.gitignore`.
- If using pkg-config, prefer imported targets.

### SDK cleanup (MANDATORY before committing)

Vendor SDKs ship with files we don't need that bloat the repo. After the user places the SDK in `AlpacaCore/external/<VENDOR>/`, audit the contents and remove everything that isn't required for the Linux arm64 build.

Scan the SDK directory and **remove**:
- **Windows files**: `*.dll`, `*.lib`, `*.exp`, `*.pdb`, `*.exe`, any `win/`, `win32/`, `win64/`, `windows/` directories
- **macOS files**: `*.dylib`, `*.framework`, any `mac/`, `macos/`, `osx/` directories
- **Non-arm64 Linux libraries**: any `x64/`, `x86_64/`, `amd64/`, `linux64/`, `x86/`, `i386/`, `armv6/`, `armv7/`, `armhf/` directories (we are arm64 only)
- **Demo/sample apps**: `demo/`, `sample/`, `example/`, `test/` directories, pre-built demo binaries
- **IDE project files**: `*.sln`, `*.vcxproj`, `*.xcodeproj`, `*.xcworkspace`
- **Documentation we don't need**: `*.pdf`, `*.doc`, `*.docx`, `*.chm` (unless it's the protocol spec we're using — ask the user)
- **Redundant packaging**: `*.tar.gz`, `*.zip`, `*.rpm`, `*.deb` inside the SDK
- **Python/Java/C# bindings**: unless the driver needs them (AlpacaBridge is C++ only)
- **Obsolete SDK versions**: if the SDK ships multiple versions, keep only the one we're building against

**Keep**:
- Linux arm64/armv8 libraries (`.a`, `.so`, `.so.*`)
- C/C++ header files (`*.h`, `*.hpp`) needed for compilation
- Udev rules (`*.rules`)
- Firmware files if required by the device (e.g. QHY `.img`/`.HEX` files)
- License/copyright files (legal requirement)
- Protocol documentation if it's our primary reference

After cleanup, list what was removed and what was kept, and confirm with the user before committing. Check an existing lean SDK (e.g. `AlpacaCore/external/SVBONY/` or `AlpacaCore/external/PlayerOne/`) as a reference for what a clean SDK directory looks like.

## Step 5 — Shared library packaging (if vendor has .so files)

**Camera vendors MUST ship .so in the .deb — non-negotiable.** Non-camera vendors also ship .so if the SDK provides one.

Checklist:
1. Store `.a` and `.so` under `AlpacaCore/external/<VENDOR>/` in the arm64 subdir the upstream SDK uses.
2. Add `!external/<VENDOR>/**` to `AlpacaCore/.gitignore` before committing SDK files.
3. Update `debian/rules` — copy `.so*` to `$(STAGING)/usr/lib/alpacabridge/`.
4. Update `build_and_run.sh` — copy `.so*` to `/usr/local/lib/`, run `ldconfig`.
5. Update `install_alpaca_service.sh` — same as `build_and_run.sh`, keep in sync.

## Step 6 — AlpacaHTTP integration

All 9 steps required for end-to-end functionality:
1. Router registration in `AlpacaHTTP/src/http/router.cpp`.
2. Router includes with `#ifdef ALPACACORE_ENABLE_<VENDOR>` guard.
3. Config sanitization fields preserved.
4. Web UI vendor dropdown in `AlpacaHTTP/web/app.js`.
5. Web UI vendor-specific form fields.
   - **Unique vendor-prefixed `name` on EVERY field** (not just filter wheels): hidden
     vendor sections still submit, so a generic `name` like `cameraIndex`/`focuserIndex`
     collides in `FormData` with ZWO's same-named field (first in DOM wins) and the value
     you typed is silently dropped — `0` gets saved. Name non-ZWO fields like
     `playerOneCameraIndex`, `qhyCameraIndex`, `touptekFocuserIndex`, and read that exact
     name in the submit handler. Element `id`s stay descriptive; only the `name` must be
     unique. (ZWO keeps the bare names as the canonical first block.)
   - **FilterWheel vendors**: the form MUST include the standard slot UI — a slot-count
     select listing the manufacturer's actual lineup (from Question 4b) plus Custom,
     per-slot filter name dropdowns, and the advanced names textarea. Instantiate
     `createFilterwheelSlotUI({...})` in `app.js` with vendor-prefixed element IDs and
     copy the markup pattern from an existing filterwheel vendor in `index.html`.
6. Web UI index auto-numbering — for any SDK-enumeration index field (camera/focuser/
   filterwheel/rotator), add an entry to the `INDEX_FIELDS` array in `app.js`
   (`fieldId`, `vendor`, `deviceType`, `configKey`, optional `idFieldId`). That wires up
   per-`(vendor, deviceType)` auto-increment and manual-edit tracking, so a second device
   of the same vendor/type doesn't silently reuse index 0. (Distinct from the Alpaca
   device number, which auto-assigns per type already.) See "Enumeration index fields" in
   AGENTS.md.
7. Frontend validation logic.
8. Build-flag propagation from AlpacaCore to AlpacaHTTP.
9. Routing/config tests in `AlpacaHTTP/tests/`.

## Step 7 — Tests (MANDATORY — do not skip)

**This step is non-negotiable.** Every driver MUST ship with Catch2 unit tests. Do NOT consider the driver complete, do NOT move to Step 8, and do NOT declare the implementation finished until tests are written, building, and passing. If the user asks to skip tests, push back — explain that every driver in this project has tests and skipping them has caused regressions in the past.

### Test file

Create `AlpacaCore/tests/test_<vendor>_<device>.cpp`.

Use an existing test file as a template — pick the one closest to your device type:

```bash
ls AlpacaCore/tests/test_*_*.cpp
```

Reference examples by device type:
- Camera → `test_svbony_camera.cpp`, `test_zwo_camera.cpp`, `test_qhy_camera.cpp`, `test_playerone_camera.cpp`, `test_touptek_camera.cpp`
- Telescope → `test_synscan_telescope.cpp`, `test_ioptron_telescope.cpp`
- Focuser → `test_gemini_focuser.cpp`, `test_zwo_focuser.cpp`
- FilterWheel → `test_zwo_filterwheel.cpp`
- Rotator → `test_zwo_rotator.cpp`
- Switch → `test_zwo_switch.cpp`
- ObservingConditions → `test_weewx_observingconditions.cpp`

### Required test structure

Every test file must include:

```cpp
#include "catch2_compat.h"
#include <alpacacore/vendor/<vendor>/<vendor>_<device>_driver.h>
```

### Required test cases (minimum 8)

All 8 are mandatory. Read the existing tests to match the exact patterns used in this project.

1. **Defaults** `"<Vendor> <Device> Driver - Defaults"` `[<vendor>][<device>][unit]`
   - Create driver with device number 0.
   - `REQUIRE(driver.get_device_type() == alpacacore::DeviceType::<Type>);`
   - `REQUIRE(driver.get_device_number() == 0);`
   - `REQUIRE(driver.get_connected() == false);`
   - `CHECK` the default device name.
   - `CHECK` any static capability flags relevant to the device type (e.g. `get_can_abort_exposure`, `get_can_reverse`, `get_absolute`, `get_can_pulse_guide`).

2. **Device metadata** `"<Vendor> <Device> Driver - Device metadata"` `[<vendor>][<device>][unit]`
   - Create driver with a **non-zero device number** (e.g. 3) so `get_unique_id()` is distinguishable from device 0.
   - `CHECK` all of: `get_device_number`, `get_description`, `get_driver_info`, `get_driver_version`, `get_interface_version`, `get_unique_id`.
   - String values must match the driver implementation exactly — read the driver source to get the correct strings. Do NOT guess.

3. **Not connected throws with correct error code** `"<Vendor> <Device> Driver - Not connected throws"` `[<vendor>][<device>][unit]`
   - Verify that operations requiring a live connection throw `alpacacore::AlpacaException` **with `error_code() == AlpacaError::NotConnected`**.
   - ConformU checks the Alpaca error number, not just that it throws — a generic `DriverException` error code will fail ConformU.
   - Cover the device's primary operations. Examples by device type:
     - Camera: `get_gain`, `start_exposure`, `get_image_array`
     - Telescope: `get_right_ascension`, `get_tracking`, `slew_to_target_async`
     - Focuser: `get_position`, `move`, `get_temperature`
     - FilterWheel: `get_position`, `set_position`
     - Rotator: `get_position`, `move_absolute`
     - Switch: `get_switch`, `set_switch`
   - Use this pattern to verify both the exception type and error code:
     ```cpp
     try {
         driver->get_right_ascension();
         FAIL("Expected AlpacaException");
     } catch (const alpacacore::AlpacaException& e) {
         REQUIRE(e.error_code() == alpacacore::AlpacaError::NotConnected);
     }
     ```

4. **Unsupported actions** `"<Vendor> <Device> Driver - Unsupported actions"` `[<vendor>][<device>][unit]`
   - `CHECK(driver.get_supported_actions().empty());` (unless the driver defines custom actions)
   - `CHECK(driver.can_action("anything") == false);`
   - `CHECK_THROWS_AS(driver.action("test", ""), alpacacore::AlpacaException);`
   - `CHECK_THROWS_AS(driver.command_blind("test", false), alpacacore::AlpacaException);`
   - `CHECK_THROWS_AS(driver.command_bool("test", false), alpacacore::AlpacaException);`
   - `CHECK_THROWS_AS(driver.command_string("test", false), alpacacore::AlpacaException);`

5. **Device-specific behavior** `[<vendor>][<device>][unit]`
   At least one test covering behavior unique to the device type. Examples:
   - Camera: sub-exposure support (`get_sub_exposure_duration` / `set_sub_exposure_duration` throw if unsupported).
   - Telescope: target coordinate persistence (set RA/Dec, read back, verify independence), site property validation (elevation [-300,10000], latitude [-90,90], longitude [-180,180]), telescope properties (`EquatorialSystem`, `AlignmentMode`, `TrackingRates` non-empty, `SlewSettleTime` >= 0), axis rate ranges.
   - Focuser: `get_absolute`, `get_temp_comp_available`, device state telemetry shape.
   - FilterWheel: names/offsets defaults, invalid position handling.
   - Switch: `get_max_switch`, invalid switch ID handling.
   - Rotator: `get_can_reverse`, device state telemetry shape.
   - ObservingConditions: `get_average_period`, sensor description throws.

### ASCOM contract tests (ConformU alignment)

These three test cases catch the bugs that cause ConformU failures. They run without hardware and verify that the driver follows the ASCOM Alpaca specification at the error code and state machine level — not just "does it throw?"

6. **Value range validation** `"<Vendor> <Device> Driver - Value range validation"` `[<vendor>][<device>][unit]`
   - Invalid inputs must throw `AlpacaException` with `error_code() == AlpacaError::InvalidValue`, not silently normalize or throw a generic error.
   - ConformU specifically tests boundary values. Device-type examples:
     - Telescope: RA outside [0, 24), Dec outside [-90, 90], site elevation outside [-300, 10000], site latitude outside [-90, 90], site longitude outside [-180, 180], `SlewSettleTime` negative, `PulseGuide` with negative duration
     - Camera: `StartExposure` with negative duration, `BinX`/`BinY` outside [1, MaxBinX/MaxBinY], `Gain` outside [GainMin, GainMax]
     - Focuser: `Move` beyond `MaxStep`, `Move` with negative position
     - Rotator: `MoveAbsolute` outside [0, 360)
     - Switch: operations with switch ID outside [0, MaxSwitch)
   - All test files use the `require_alpaca_error` helper to verify both exception type and error code:
     ```cpp
     require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); },
                          alpacacore::AlpacaError::InvalidValue);
     require_alpaca_error([&]() { driver->set_site_elevation(10000.1); },
                          alpacacore::AlpacaError::InvalidValue);
     ```
   - Note: some drivers check connection before validating values (e.g., iOptron site properties throw NotConnected first). Only test value validation for operations that work disconnected — check existing tests for the same device type.

7. **State machine contracts** `"<Vendor> <Device> Driver - State machine"` `[<vendor>][<device>][unit]`
   - Verify that device state follows ASCOM rules without needing hardware.
   - Camera: `CameraState` is `Idle` before any exposure, `ImageReady` is false (or throws NotConnected) before any exposure, `IsPulseGuiding` is false when idle.
   - Telescope: `Slewing` is false when not connected, `IsPulseGuiding` is false when idle, `Tracking` default state is correct.
   - Focuser: `IsMoving` is false when idle.
   - These state machine tests have caught real bugs in past drivers:
     - iOptron: settle loop prematurely declared slews complete
     - SynScan: `IsPulseGuiding` always returned false
     - SVBONY: `CameraState` got stuck after SDK hangs

8. **Unsupported method error codes** `"<Vendor> <Device> Driver - Unsupported methods"` `[<vendor>][<device>][unit]`
   - Methods the device doesn't support must throw `AlpacaException` with the correct error code — usually `AlpacaError::InvalidOperation` or `MethodNotImplemented`, NOT a generic `DriverException`.
   - ConformU distinguishes between "not implemented" and "driver error" — the wrong error code fails validation.
   - Examples:
     - Telescope without `CanSyncAltAz`: `SyncToAltAz` must throw with correct code
     - Camera without shutter: `HasShutter` returns false, operations should behave accordingly
     - Focuser without temp comp: `set_temp_comp(true)` must throw with correct code

### CMake integration (also mandatory)

Update `AlpacaCore/tests/CMakeLists.txt`:
- Add `test_<vendor>_<device>.cpp` to the conditional `TEST_SOURCES` list, guarded by `if(TARGET alpacacore_<vendor>)`.
- Add `target_link_libraries(alpacacore_tests PRIVATE alpacacore_<vendor>)` in the matching conditional block.

Follow the existing pattern in the file — read it first:
```bash
cat AlpacaCore/tests/CMakeLists.txt
```

### Build and run the tests

After writing the tests, build and run them immediately. Do not defer this:

```bash
cd AlpacaCore && cmake -B build -DALPACACORE_ENABLE_<VENDOR>=ON && cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests "[<vendor>]"
```

If any test fails, fix it before proceeding. The test tag filter `"[<vendor>]"` runs only the new vendor's tests for faster iteration.

### Assertion count target

Aim for **at least 30 assertions** across the 8 test cases. The ASCOM contract tests (cases 6-8) should add 10-15 assertions on top of the basic 5 cases. If you have significantly fewer, you're probably not testing enough error codes and state transitions.

## Step 8 — Full build and verify

Build the complete test suite (not just the new vendor) to catch any regressions:

```bash
cd AlpacaCore && cmake -B build -DALPACACORE_ENABLE_<VENDOR>=ON && cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests
```

All tests must pass — not just the new driver's tests.

## Step 9 — Vendor-specific notes

Before implementing, read AGENTS.md for any vendor-specific notes that apply. Check:
```bash
grep -A 50 "### <VendorName>" AGENTS.md
```

Apply any vendor-specific quirks, workarounds, or conventions documented there.

## Step 10 — ConformU validation (MANDATORY — both platforms)

After the driver builds and unit tests pass, the user MUST run ConformU against the driver with real hardware on **both target platforms**. A driver is NOT complete until it has ConformU results for both architectures.

**ConformU**: https://github.com/ASCOMInitiative/ConformU — the official ASCOM conformance test suite. Current version: 4.3.0.

### Required validation matrix

Every driver must be validated on **arm64**:

| Platform | Architecture | Example hardware |
|----------|-------------|-----------------|
| Linux arm64 | 64-bit ARMv8 | Raspberry Pi 4/5, OrangePi, Rockchip SBCs, iOptron iMate |

**arm64 only** — no amd64/x86_64, no 32-bit (x86, armv7, armhf).

arm64 is not just Raspberry Pi — the project supports a growing range of arm64 SBCs and embedded astronomy computers. See the OpenAstro docs for the current list: https://www.openastro.net/docs/sbc-install/overview

### Validation steps

1. Build AlpacaBridge with the new driver enabled.
2. Start AlpacaBridge with the new driver configured and connected to real hardware.
3. Run ConformU against the device. Target: **0 errors, 0 issues, 0 timing issues**.
4. **Verify the Timing Summary** at the end of the ConformU log — this is a separate pass criterion from the main "no errors, warnings or issues found" message. A clean run MUST satisfy all of:
   - No `OUTSIDE FAST RESPONSE TIME TARGET` lines
   - No `OUTSIDE STANDARD RESPONSE TIME TARGET` lines
   - No `OUTSIDE EXTENDED RESPONSE TIME TARGET` lines
   - No trailing `N member(s) took longer than its target response time.` summary line
   - If a JSON report (`conform.report.txt`) is produced, `"TimingIssuesCount": 0`

   Per-spec response time targets (reported in the Timing Summary header):
   - **FAST** — 0.1 seconds (configuration and state reporting members, e.g. `Name`, `Connected`, `CanXxx`, position reads)
   - **STANDARD** — 1.0 second (property writes and asynchronous initiators, e.g. `Tracking Write`, `SlewToCoordinatesAsync`)
   - **EXTENDED** — 600.0 seconds (synchronous methods, `ImageArray`, `ImageArrayVariant`)

   Quick check after a run:
   ```bash
   grep -E "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET|took longer than its target response time" <conformu.log>
   grep '"TimingIssuesCount"' <conform.report.txt>   # if JSON report is produced
   ```
   Both must return either nothing or `"TimingIssuesCount": 0`.

   If any member exceeds its target, treat it as a failure. Common root causes (see "Common ConformU failure patterns" below for the full list):
   - Slow SDK control writes — defer to start of operation or cache
   - Network I/O while holding the driver mutex — release before I/O, re-acquire after
   - Repeated round-trips for cached values (RA/Dec, capabilities) — add a short TTL cache
   - First-call SDK warm-up on `Name`/identity reads — warm up at connect

5. Save the ConformU results to `AlpacaCore/conformu/<vendor>/<model>/Linux-arm64.txt`.
   - If the device supports multiple connection types, save separate results: `Linux-arm64-usb.txt`, `Linux-arm64-wifi.txt`.
   - If ConformU also produced a JSON report, save it alongside the text log (e.g. `Linux-arm64.report.json`) so the timing data is preserved.
6. If ConformU reveals failures (errors, issues, OR timing issues), fix them and re-run until clean.

### After validation — update SUPPORTED-DRIVERS.md (MANDATORY)

Read the current `SUPPORTED-DRIVERS.md` and add or update the entry for the new driver. Follow the existing format. Required information:

1. **Driver table entry** — add a row in the appropriate device type section with:
   - Model name
   - Connection type (USB, Wi-Fi, Serial, TCP)
   - ConformU link (relative path to the results file)
   - Validated platform (Linux arm64)
   - ConformU version used

2. **Driver Notes section** — add or update the vendor's driver notes with:
   - SDK version or protocol spec version
   - Tested firmware version(s) and model(s)
   - Connection types tested
   - Any notable quirks or limitations for end users
   - Verified OS/Architecture list

3. Also update `AGENTS.md` vendor-specific notes if any quirks or workarounds were discovered during validation.

### Common ConformU failure patterns from past drivers

- **SideOfPier** returning raw mount value instead of ASCOM convention (hour-angle based).
- **IsPulseGuiding** always returning false instead of tracking actual pulse state.
- **Slew completion** declared too early via position tolerance shortcuts — trust the mount's status register.
- **MoveAxis tertiary axis** (axis=2) should throw `InvalidValue`, not `MethodNotImplemented`.
- **AxisRates** returning wrong structure — must be vector of rate ranges per ASCOM spec.
- **Property writes after alignment** corrupting mount pointing model (site lat/lon, UTC date).
- **SDK control writes** taking >1 second and blowing ASCOM client timing budgets — defer to start of operation.
- **Wi-Fi vs USB differences**: a driver that passes ConformU over USB may fail over Wi-Fi due to timing, stale bytes, or mutex contention. Test both connection types if the device supports them.

## Lessons from past driver implementations

These are recurring issues that have burned time across multiple drivers. Check each one against the device type you're building.

### Telescope-specific pitfalls

Every telescope driver in this project has hit most of these:

1. **SideOfPier ASCOM convention**: The raw pier side value from the mount almost never matches the ASCOM convention. ASCOM defines `pierEast` (0) when HA >= 0, `pierWest` (1) when HA < 0. SynScan, iOptron, and Celestron all needed fixes for this. Compute from hour angle (LST - RA), don't return raw mount values.

2. **Pulse guiding is always harder than expected**:
   - If the mount has no native pulse guide command (SynScan), implement software-timed variable-rate slew: issue slew at guide rate, sleep for duration, stop axis, restore tracking.
   - If the mount has native pulse guide (iOptron `:ZS#`/`:ZQ#`/`:ZE#`/`:ZC#`, Celestron MC_AUX_GUIDE 0x26), use it — hardware timing is more accurate.
   - `IsPulseGuiding` must return actual status via time-based end tracking, not always false.
   - **Cross-axis hold**: freeze the non-guiding axis position at the pre-pulse value during the guide window. Without this, cos(DEC) amplification at high declinations causes ConformU tolerance failures.
   - **RA tracking restoration**: after stopping an RA-axis pulse, re-issue the tracking mode command. Variable-rate stop commands kill sidereal tracking on many mounts.
   - **Position override accumulation**: for software-timed pulse guides, accumulate expected `rate × duration` deltas instead of reading back noisy mount positions. Eliminates drift between reads.

3. **GEM DEC direction flip**: DEC motor direction inverts when the mount is on the west side of the pier. Every GEM telescope driver needs this.

4. **Sync must use mount's native sync command**: Do NOT maintain driver-level sync offsets — they cause coordinate divergence during slews because the mount doesn't know about them. Use the mount's own calibration command (iOptron `:CM#`, Celestron `S`/`s`, etc.).

5. **Post-slew tracking restoration**: After a GOTO completes, re-issue the tracking mode command to ensure sidereal tracking resumes. Some mounts stop tracking during slews and don't automatically resume.

6. **Stale serial buffer bytes**: Leftover bytes from previous responses can contaminate critical command responses (e.g., slew commands). Call `flush_input()` (via `tcflush`/`PurgeComm`) before issuing slew or status commands.

7. **Auto-detection pattern**: Scan `/dev/serial/by-id/` first (more stable names), then fall back to `/dev/ttyUSB*`. Probe each port with the mount's firmware/version query. Connect to the first responding mount. Look for known USB-serial chip vendor IDs (Prolific, FTDI, CP210x, CH340/CH341, Silicon Labs).

### Camera-specific pitfalls

1. **SDK singleton lifecycle**: Camera SDKs (ZWO, QHY, SVBONY, PlayerOne) require global init/release calls. Manage as a singleton in the wrapper — call init once, release on shutdown. Include any required preprocessor defines (e.g., QHY needs `#define __CPP_MODE__ 1`).

2. **Control warm-up**: Some cameras (SVBONY SV905C2) reject control writes until the SDK has been warmed up. The SVBONY driver works around this by writing every writable control to its default value during connect.

3. **Guide direction mapping**: ASCOM defines North=0, South=1, East=2, West=3. Some SDKs differ (QHY: East=0, North=1, South=2, West=3). Map explicitly.

4. **ROI alignment**: Most camera SDKs require width to be a multiple of 4 or 8, and height a multiple of 2. Accept any user value for the Alpaca interface, internally align down for SDK calls, and zero-pad the output if needed.

5. **libusb conflicts**: ZWO and QHY both statically link libusb. Linking both into the same binary causes duplicate symbol issues. The ZWO vendor CMakeLists.txt handles this — be aware when adding camera vendors.

6. **Firmware and udev requirements**: Some cameras (QHY) need firmware files installed at `/lib/firmware/` and custom udev rules. The system `fxload` may not support all chip types — use the SDK's own `fxload` binary if provided.

### Network/Wi-Fi pitfalls

1. **Stale TCP bytes**: Blind commands (no response expected) can still produce acknowledgment bytes that accumulate in the TCP receive buffer. Over Wi-Fi, this eventually overwhelms the mount's Wi-Fi module. Drain stale bytes after each blind command using non-blocking `poll()`/`select()`.

2. **Don't hold mutex during network I/O**: Network operations can take hundreds of milliseconds over Wi-Fi. If the driver mutex is held during these operations, other threads (like IsPulseGuiding) block and miss timing targets. Release the mutex before network I/O, re-acquire after.

3. **Position cache TTL**: Over high-latency Wi-Fi links, cache RA/Dec reads for a few seconds (e.g., 5s TTL) so rapid-fire position queries from clients don't each trigger a network round-trip.

4. **TCP_NODELAY**: Set `TCP_NODELAY` on the socket to avoid Nagle's algorithm adding latency to small command packets.

### Serial port pitfalls

1. **DTR/HUPCL on CH340/CH341**: These USB-serial adapters assert DTR on port open, which resets Arduino/ESP32 MCUs. During auto-detect probe, clear HUPCL before closing the port so DTR stays high. Without this, the MCU double-resets with a ~4 second penalty.

2. **Baud rate matters**: Don't assume 9600. iOptron uses 115200 by default (v3.10 spec). Some devices are configurable. Check the protocol spec.

3. **No hash terminator**: Some commands don't terminate responses with `#` (iOptron `:MountInfo#` returns exactly 4 bytes, no terminator). Use idle-timeout read mode for these.

### Focuser-specific pitfalls

1. **Step size**: Some focuser protocols don't expose step size in microns — it varies by mechanical configuration. Throw `PropertyNotImplemented` rather than returning 0.0.

2. **Movement commands are often fire-and-forget**: The move command returns immediately. Poll `is_moving()` to detect completion.

3. **Motor speed**: Set motor speed to fast on connect so moves complete within ConformU's 60-second timeout.

## Step 11 — Update AGENTS.md with lessons learned (MANDATORY)

After the driver is implemented, tested, and validated, update `AGENTS.md` with any new knowledge discovered during the session. **This step is non-negotiable** — AGENTS.md is the project's living knowledge base and must grow with every driver session.

### What to add

1. **New vendor section** — If this is a new vendor, add a `### <Vendor>` section under `## Vendor-Specific Notes` following the existing format:
   - Devices supported
   - SDK locations or protocol documentation paths
   - Connection types
   - All quirks, workarounds, and non-obvious behaviors discovered

2. **Existing vendor updates** — If extending an existing vendor, add any new findings to the existing section:
   - New device type support
   - New quirks or workarounds found
   - Updated ConformU validation status (model, platform, version)
   - Firmware-specific behaviors

3. **General notes** — If a lesson applies across vendors (e.g., a new ConformU pattern, a serial port quirk, a packaging gotcha), add it to the appropriate general section.

### What qualifies as a lesson learned

- Any behavior that was surprising or non-obvious
- Any workaround that was needed for hardware/SDK/protocol quirks
- Any ConformU failure that required a code change
- Any timing or threading issue that was discovered
- Any protocol ambiguity that was resolved by testing
- Any SDK bug or undocumented behavior
- Any arm64-specific issue (SBC firmware quirk, kernel version requirement, etc.)

### How to update

Read the current AGENTS.md, find the right section, and add the new notes. Follow the existing format — look at the SynScan, iOptron, Celestron, or SVBONY sections for examples of well-documented vendor notes.

If in doubt about whether something is worth documenting, document it. A note that saves 30 minutes of debugging next time is always worth the two lines it takes to write.

## Workflow

Work through steps sequentially. After each step, briefly report what was done and confirm before moving to the next. If the user wants to skip a step (e.g., no .so files to package), acknowledge and move on.

Use the logging sink flow from AlpacaCore — no ad-hoc stdout/stderr. Use `AlpacaException` for error paths. Protect shared state with mutexes. Avoid global mutable state.
