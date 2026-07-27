---
description: Run ConformU against a connected AlpacaBridge device, validate the results (errors, issues, timing), save the logs, and update SUPPORTED-DRIVERS.md
argument-hint: [vendor] [model]
allowed-tools: Read, Edit, Write, Bash, Grep, Glob
---

You are the ConformU test session assistant for AlpacaBridge. Your job is to drive a single ConformU run against a device that is already configured and connected in a running AlpacaBridge instance, validate the result against the project's strict pass criteria (errors, issues, AND timing), then save the logs and update `SUPPORTED-DRIVERS.md`.

You DO NOT start or stop AlpacaBridge — that is the user's responsibility. You DO NOT connect the device — that is configured via the web UI. You only run ConformU and handle its output.

**Is the target running the build under test?** ConformU results are only meaningful when the target device runs the build being validated. If the code under test was just built locally and the target SBC hasn't been updated, tell the user to run `/deploy-test` first — it builds the .deb, installs it on the SBC over SSH, restarts the service, and verifies the running version — then come back to `/conformu`.

## How to use arguments

The user invoked this command with: $ARGUMENTS

- If `$ARGUMENTS` includes a vendor and a model (e.g. `iOptron HEM27`), use them as defaults in Step 1 — still confirm with the user.
- If only a vendor is provided, ask for the model.
- If no arguments are provided, ask for everything in Step 1.

## Step 1 — Gather session inputs

Ask the following **one at a time**, waiting for each answer before moving on. If `$ARGUMENTS` already covers a value, present it as the default and ask for confirmation rather than re-asking from scratch.

1. **Vendor** — must match an existing vendor directory under `AlpacaCore/conformu/` if one exists. Get the canonical list with:
   ```bash
   ls -1 AlpacaCore/conformu/ | grep -v '\.md$'
   ```
   Use the existing case exactly (e.g. `Player One`, not `PlayerOne`; `iOptron`, not `IOptron`). If the vendor is new, confirm the canonical name the user wants for the directory.

2. **Model / device name** — the human-readable model (e.g. `HEM27`, `Ceres 462M`, `ASI2600MC Pro`). This becomes the directory name under the vendor. If the vendor already has subdirectories, list them so the user can match the existing naming style.

3. **ASCOM device type** — Camera, CoverCalibrator, Dome, FilterWheel, Focuser, ObservingConditions, Rotator, SafetyMonitor, Switch, Telescope. Needed to build the Alpaca URI path segment (`camera`, `telescope`, etc. — lowercase).

4. **Connection transport** — USB, Wi-Fi, Serial, TCP, or GPIO. Used as the filename suffix and as the connection cell in `SUPPORTED-DRIVERS.md`. The on-disk suffix is lowercase (`usb`, `wifi`, `serial`, `tcp`, `gpio`). The table cell uses display form (`USB`, `Wi-Fi`, `Serial`, `TCP`, `Local GPIO (libgpiod v2)`). Use `GPIO` for devices that AlpacaBridge controls via on-board Linux GPIO lines on the host SBC (e.g. ZWO ASIair Pro 12V power switch on a Raspberry Pi 4).
   - If the device only supports one transport, omit the suffix — file is `Linux-arm64.txt`.
   - If the device supports multiple and this run only tests one, use the suffix — file is `Linux-arm64-<transport>.txt`. Existing reports already exist for the same device — those other transports stay untouched.

5. **Alpaca host:port** — defaults to `http://localhost:11111`. Ask if they want a different host (e.g. testing against a remote SBC).

6. **Device number** — defaults to `0`. Most single-device setups stay at 0.

After all answers, summarize and confirm before continuing:

> "Ready to run ConformU against `<vendor> <model>` (<type>) over <transport> at `http://<host>:<port>/api/v1/<type>/<n>`. Output will be saved to `AlpacaCore/conformu/<Vendor>/<Model>/Linux-arm64[-<transport>].txt`. Proceed?"

## Step 2 — Pre-flight checks (HARD STOP if any fail)

Before invoking ConformU, verify the environment. If any check fails, **STOP** and tell the user what to fix. Do NOT run ConformU against a broken setup — it wastes time and produces noisy logs that look like driver bugs.

### 2a. AlpacaBridge reachability

```bash
curl -sS --max-time 5 http://<host>:<port>/management/v1/description | head -c 500
```

Expect a JSON response containing `ServerName` or `Manufacturer`. If the request times out or returns an error:

> "AlpacaBridge is not reachable at `http://<host>:<port>`. Start it (`./build_and_run.sh`) and re-run `/conformu`."

### 2b. Device is configured

```bash
curl -sS --max-time 5 "http://<host>:<port>/api/v1/<type>/<n>/name?ClientID=1&ClientTransactionID=1"
```

Expect a JSON body with `Value` set to a non-empty string and `ErrorNumber: 0`. If `ErrorNumber` is non-zero or the request 404s, the device isn't configured at that number:

> "No `<type>` device is configured at index `<n>` on `http://<host>:<port>`. Configure it via the web UI (`http://<host>:<port>/`) and re-run."

### 2c. Device is connected

```bash
curl -sS --max-time 5 "http://<host>:<port>/api/v1/<type>/<n>/connected?ClientID=1&ClientTransactionID=1"
```

Expect `Value: true`, `ErrorNumber: 0`. If `Value: false`:

> "Device `<vendor> <model>` is configured but not connected. Connect it via the web UI before running ConformU."

If `ErrorNumber` is non-zero (e.g. NotConnected because hardware is unplugged), report the exact error and stop.

### 2d. Capture the current AB log level (so we can restore it later)

```bash
curl -sS --max-time 5 "http://<host>:<port>/management/v1/loglevel?ClientID=1&ClientTransactionID=1" \
  | jq -r '.Value | fromjson | .Level'
```

Save this value (e.g. `INFO`) into a session variable — Step 8 restores it. The log level is persisted on disk, so a crash mid-skill would leave AB at TRACE forever; restoring is mandatory on every exit path including failure.

### 2e. Set AB log level to TRACE

```bash
curl -sS --max-time 5 -X PUT "http://<host>:<port>/management/v1/loglevel" \
  -H "Content-Type: application/json" \
  -d '{"Level":"TRACE","ClientID":1,"ClientTransactionID":1}'
```

Verify the response shows `Level: TRACE`. TRACE includes every driver call, SDK round-trip, and HTTP request — exactly what we need to diagnose any ConformU failure.

### 2f. Capture the AB log file path and a pre-run baseline timestamp

```bash
curl -sS --max-time 5 "http://<host>:<port>/management/v1/logfiles?ClientID=1&ClientTransactionID=1" \
  | jq -r '.Value | fromjson | .Directory'
```

Save the directory. Today's log file is named per the daily convention (check the Files list in the same response). Record `date -u +%H:%M:%S.%3N` as the **run-start baseline** — used in Step 5 to slice the log to just this test window.

### 2g. ConformU binary is available

Search common locations in this order, stop at the first match:
```bash
command -v conformu
ls /home/dev/Downloads/conformu/conformu
ls /opt/conformu/conformu
ls ~/conformu/conformu
```

If none found:

> "ConformU binary not found. Download from https://github.com/ASCOMInitiative/ConformU/releases (linux-arm64) and extract to `~/conformu/` or `/home/dev/Downloads/conformu/`."

### 2h. ConformU is the CURRENT release (check upstream before every run)

Validation logs advertise the ConformU version they were produced with, so always test with the latest release. Get the installed version and the newest upstream tag:

```bash
<path-to-conformu>/conformu --version
curl -sS --max-time 10 "https://api.github.com/repos/ASCOMInitiative/ConformU/releases/latest" | jq -r '.tag_name'
```

(If the API call fails — offline, or the 60/hr unauthenticated rate limit when `/conformu` runs repeatedly in a session — fall back to checking https://github.com/ASCOMInitiative/ConformU/tags, and if that's also unreachable, warn the user and proceed with the installed version. When `gh` is authenticated, prefer `gh api repos/ASCOMInitiative/ConformU/releases/latest --jq .tag_name` to avoid the unauthenticated limit entirely.)

Compare the two **numerically per dot-segment, never lexicographically** — a string compare misjudges `4.9.0` vs `4.10.0`. Strip any `v` prefix, then:

```bash
# Prints the higher of the two versions
printf '%s\n%s\n' "<installed>" "<latest>" | sort -V | tail -1
```

The installed version is current if and only if it equals the `sort -V` maximum:

- **Installed == latest** → note the version and continue.
- **Installed < latest** → offer to update before running:

  > "ConformU `<installed>` is outdated — `<latest>` is available. Update now? I'll download the linux-arm64 build and replace the current install."

  On yes, download the `linux-arm64` asset from the latest release, extract it over the existing install location found in Step 2g, make the binary executable, and re-run `conformu --version` to confirm the new version before continuing. On no, proceed but record the older version accurately in the summary table and `SUPPORTED-DRIVERS.md` notes — never label a log with a version that wasn't used.

## Step 3 — Run ConformU

Create a temp working directory and invoke the conformance subcommand:

```bash
TMPDIR=$(mktemp -d /tmp/conformu-XXXXXX)
<path-to-conformu>/conformu conformance \
  "http://<host>:<port>/api/v1/<type>/<n>" \
  -n "$TMPDIR/conformu.txt"
```

Stream output to the user as it runs. ConformU can take minutes (especially for cameras and telescopes). Do not interrupt unless the user says so.

If the run errors out before producing a log (e.g. SIGSEGV, missing .NET runtime), surface the error message verbatim and stop — do not proceed to validation.

## Step 4 — Validate results (HARD GATE)

Validation reads the ConformU text log only. A pass requires both assertions clean. This mirrors `/driver-build` Step 10 and the `/commit` hard-block.

### 4a. Text log — pass message present

```bash
grep -q "Congratulations, no errors, warnings or issues found" "$TMPDIR/conformu.txt"
```

**Fail** if absent. This assertion is ConformU's own confirmation that every member returned `ErrorCount=0`, `IssueCount=0`, and `WarningCount=0` — no separate count files are needed to derive it.

### 4b. Text log — no timing violations

```bash
grep -nE "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" "$TMPDIR/conformu.txt"
grep -n "took longer than its target response time" "$TMPDIR/conformu.txt"
```

**Fail** if either grep matches. A clean run also emits `Congratulations, all members returned within their target response times!!` — its presence is a positive signal but its absence alone is not a failure (the two greps above are authoritative).

### 4c. Show a summary table

Regardless of pass/fail, show the user a concise summary derived from the text log:

```
ConformU <version> — <vendor> <model> (<type>) over <transport>
  Errors / Issues / Warnings:  0 / 0 / 0    (from pass message)
  Timing violations:           0            (no OUTSIDE … RESPONSE TIME TARGET lines)
  Slowest member:              Name (0.409s, target 0.1s)   ← only if any timing issue
  Pass message:                Present
  Verdict:                     PASS / FAIL
```

The slowest-member line is helpful even on a pass — call it out if any member came within 80% of its target (`grep "(FAST)\|(STANDARD)\|(EXTENDED)"` and pick the closest-to-limit row).

## Step 5 — On failure — diagnose and fix the driver

Do NOT save the failing ConformU log to `AlpacaCore/conformu/`. That directory is the source of truth for `SUPPORTED-DRIVERS.md` and must not contain polluted results.

### 5a. Show the failure summary

1. The summary table from Step 4d.
2. The specific failing operations:
   - For errors/issues — extract matching lines from `$TMPDIR/conformu.txt`.
   - For timing — list each `OUTSIDE … RESPONSE TIME TARGET` line with member name, actual time, and target.

### 5b. Pull the AlpacaBridge TRACE log for the test window

The TRACE log captured during Step 3 is the most valuable diagnostic. Retrieve it and slice to the test window (baseline timestamp from Step 2f → now):

```bash
curl -sS --max-time 10 "http://<host>:<port>/management/v1/logs?ClientID=1&ClientTransactionID=1" \
  | jq -r '.Value' > "$TMPDIR/ab.log"
```

If the file is large, also keep a sliced copy for analysis:

```bash
awk -v start="<run-start-baseline>" '$0 ~ start, /./' "$TMPDIR/ab.log" > "$TMPDIR/ab.window.log"
```

(If the awk slice is empty because the timestamp format doesn't match line-for-line, fall back to using the full log.)

### 5c. Correlate ConformU failures with AB log entries

For each failing operation in the ConformU log, extract its timestamp (`HH:MM:SS.fff`) and the operation name (`SlewToCoordinates`, `Tracking Write`, `Name`, `PulseGuide East 2s`, etc.). Then:

```bash
# Find AB log lines within ~2 seconds of the failing operation, mentioning the operation or its underlying driver method
grep -n "<HH:MM:SS>" "$TMPDIR/ab.window.log" | head -50
grep -nE "<driver_method_pattern>" "$TMPDIR/ab.window.log" | head -50
```

For each failing operation, build a short evidence block — quote the matched AB log lines (timestamps, component, message) so the diagnosis is grounded in actual driver behavior, not guesses.

### 5d. Root-cause analysis

Map evidence to a likely root cause. Common patterns (the same list as `/driver-build` Step 10, now informed by the TRACE log):

| Symptom in ConformU | Look for in AB TRACE log | Likely root cause |
|---------------------|--------------------------|-------------------|
| Slow `Name` (>0.1s FAST) | First call into SDK takes long; subsequent calls fast | Cold SDK warm-up — call identity once during connect |
| Slow position read (RA, Dec, Position, Temperature) | Each read triggers a serial/TCP round-trip | Add short TTL cache in the protocol/SDK wrapper |
| Slow property writes (Tracking Write, Site* Write, GuideRate*) | Long mutex hold spanning network I/O | Release driver mutex around the network call |
| Slow async initiator (SlewToCoordinatesAsync, PulseGuide) | Initiator blocks on full SDK call | Defer SDK work to a background task |
| `OUTSIDE EXTENDED` on slew/exposure | Polling loop running too slow or sleeping too long | Tighten poll interval; trust the mount/SDK status register |
| `InvalidValue` expected but got `DriverException` | Wrong exception type thrown in driver | Use `AlpacaException(AlpacaError::InvalidValue, ...)` |
| `MethodNotImplemented` expected but got `DriverException` | Capability check missing or wrong code | Return correct `AlpacaError` for unsupported features |
| State machine mismatch (Slewing, IsPulseGuiding, CameraState) | Flag never set/cleared; race condition | Audit the state transitions and mutex coverage |

For each failing operation, state the diagnosis as: **`<member>` failed with `<symptom>`. AB TRACE shows `<quoted evidence>`. Root cause: `<one-line>`. Fix: `<specific change to specific file>`.**

### 5e. Locate the driver source and propose the fix

```bash
ls AlpacaCore/src/vendors/<vendor>/
ls AlpacaCore/include/alpacacore/vendor/<vendor>/
```

For each diagnosed issue, identify the exact file + function + lines to change, and draft the patch. Show the proposed diff to the user (with surrounding context) and ask for approval before applying.

Honor the project rules from `/driver-build`:
- Don't touch the Alpaca interface header (`AlpacaCore/include/alpacacore/<device>_driver.h`) unless the spec genuinely demands it.
- Driver `.cpp` calls the wrapper; the wrapper does protocol/SDK I/O. Don't add raw SDK calls to the driver.
- AGPL license headers stay intact.
- Don't add error handling or fallbacks for scenarios that can't happen — fix the actual cause.

### 5f. Apply the fix

After user approval, apply each edit. If multiple files are touched, group them logically. Run the unit tests for the vendor:

```bash
cd AlpacaCore && cmake --build build --target alpacacore_tests && ./build/tests/alpacacore_tests "[<vendor>]"
```

If unit tests pass, tell the user:

> "Fix applied to `<files>`. Unit tests pass. To verify against hardware: rebuild AlpacaBridge (`./build_and_run.sh`), wait for the device to reconnect, then re-run `/conformu <vendor> <model>`. The log level is still TRACE — Step 8 will restore it after the next successful run, or run `/conformu` again to keep iterating."

Do NOT rebuild AlpacaBridge from this skill (matches the "assume running" decision in Step 2). Do NOT auto-restart the process. The user owns AB's lifecycle.

### 5g. If diagnosis is unclear

If TRACE evidence is thin or contradictory, do NOT guess a fix. Tell the user honestly:

> "ConformU shows `<failure>` but the AB TRACE log doesn't clearly explain it. I can see `<observations>`. Options: (1) add more logging in `<driver function>` and re-run, (2) reproduce manually with curl against the same endpoint, (3) check existing INDI/INDIGO drivers for how they handle this method. Which would you like?"

Do NOT save the failing ConformU log. Do NOT update SUPPORTED-DRIVERS.md. Stop after the diagnosis is delivered.

## Step 6 — On pass — sanity-check AB logs, then save

### 6a. Scan AB TRACE log for hidden warnings or errors

Even when ConformU passes, the AB log can reveal latent issues — silently swallowed exceptions, retried SDK calls, surprise reconnects. Fetch the log and grep:

```bash
curl -sS --max-time 10 "http://<host>:<port>/management/v1/logs?ClientID=1&ClientTransactionID=1" \
  | jq -r '.Value' > "$TMPDIR/ab.log"
grep -nE "WARN|ERROR|CRITICAL|exception|retry|timeout|reconnect" "$TMPDIR/ab.log" | head -30
```

If matches appear, show them to the user. A passing ConformU with a stack trace in the AB log is still a yellow flag — ask the user whether to proceed with saving or to investigate first. Default to proceeding only after the user confirms the matches are benign (e.g. expected NotConnected errors during a controlled disconnect test).

### 6b. Save logs

Compute the destination directory:

```
AlpacaCore/conformu/<Vendor>/<Model>/
```

- `<Vendor>` uses the canonical case from Step 1.
- `<Model>` uses the canonical case from Step 1.
- Some vendors group by SDK family (e.g. `ZWO/ASI/`, `ZWO/EAF/`). Check existing subdirs under the vendor before creating a new top-level model dir; ask the user if a grouping subdir applies.

Filenames:
- Single transport: `Linux-arm64.txt`
- Multiple transports: `Linux-arm64-<transport>.txt` (lowercase: `usb`, `wifi`, `serial`, `tcp`, `gpio`)

```bash
mkdir -p "AlpacaCore/conformu/<Vendor>/<Model>"
cp "$TMPDIR/conformu.txt" "AlpacaCore/conformu/<Vendor>/<Model>/Linux-arm64[-<transport>].txt"
```

The text log alone is sufficient: it carries the `Congratulations, no errors, warnings or issues found` assertion (covers Errors / Issues / Warnings) and any `OUTSIDE … RESPONSE TIME TARGET` lines (covers timing). Do **not** generate or save a JSON report — the `-r` flag was dropped from the ConformU invocation in Step 3 precisely so the text log is the single source of truth and there is no parallel JSON to drift out of sync.

If a file with the same name already exists, show the user the new pass message and any timing lines vs the old, and ask whether to overwrite. Overwriting a passing result with another passing result is usually fine (e.g. re-validating against a newer ConformU); overwriting a passing result for a different transport is a mistake.

## Step 7 — Update SUPPORTED-DRIVERS.md

Read `SUPPORTED-DRIVERS.md` and locate the section for this device type (`## Camera Drivers`, `## Telescope Drivers`, etc.).

### 7a. Table row

Find the vendor's `### <Vendor>` subsection in that device-type section. If absent, create a new vendor subsection following the existing format:

```
### <Vendor>

| Model Series | Connection | Linux<br>(arm64) | Status |
|--------------|------------|------------------|--------|
| <Model> | <Transport> | ✓ | [ConformU Validation](AlpacaCore/conformu/<Vendor>/<Model>/) |
```

URL-encode spaces in the path link as `%20` (e.g. `Player%20One`, `Ceres%20462M`).

If a row for this exact model already exists:
- If it was previously `pending arm64 re-validation` or had no ✓, update the row to add the ✓ and the link.
- If it already had ✓ and a link, no change needed (the log file was just refreshed).
- If the connection cell needs to gain a second transport (e.g. `USB` → `USB, Wi-Fi`), update it.

### 7b. Driver Notes

After the table, find the `### <Vendor> Driver Notes` (or `### <Vendor> <DeviceType> Driver Notes`) section. If absent, add one in the existing format:

```
### <Vendor> <DeviceType> Driver Notes

- **SDK**: <SDK name + version> (or **Protocol**: <spec version> for non-SDK devices)
- **Connection**: <Transport> (<details, e.g. baud rate, default IP>)
- **Tested model**: <Model> on Linux arm64
- **ConformU**: <version> — 0 errors, 0 issues, 0 timing issues
```

If the notes already exist, append a new tested-model line or update the existing one — do not duplicate. Ask the user for SDK/protocol version details if you don't already have them from the session.

### 7c. Updated date

Update the `## Updated YYYY-MM-DD` line near the top of the file to today's date.

### 7d. Confirm before writing

Show the user the exact diff that will be applied (added row, added Driver Notes lines, updated date) and confirm before saving.

## Step 8 — Restore the original AB log level (MANDATORY — every exit path)

Whether the run passed, failed, or was aborted mid-flow, restore the log level captured in Step 2d. The persisted setting would otherwise leave AB at TRACE indefinitely, bloating disk and slowing all subsequent operations.

```bash
curl -sS --max-time 5 -X PUT "http://<host>:<port>/management/v1/loglevel" \
  -H "Content-Type: application/json" \
  -d "{\"Level\":\"<original-level>\",\"ClientID\":1,\"ClientTransactionID\":1}"
```

If AB is unreachable at this point (crashed during the test), tell the user explicitly:

> "Could not restore AB log level — AlpacaBridge is not reachable. When you restart AB, run: `curl -X PUT http://<host>:<port>/management/v1/loglevel -H 'Content-Type: application/json' -d '{\"Level\":\"<original-level>\"}'`"

This restore step runs at the end of every Step 5 failure path AND at the end of every Step 6/7 success path. Do not skip it.

## Step 9 — Hand off to /commit (pass path only)

After files are saved and `SUPPORTED-DRIVERS.md` is updated, tell the user:

> "ConformU validated — 0 errors, 0 issues, 0 timing issues. Saved to `AlpacaCore/conformu/<Vendor>/<Model>/`. SUPPORTED-DRIVERS.md updated. AB log level restored to `<original-level>`. Run `/commit` to stage and commit these changes — the hard-block in `/commit` Step 3 will re-verify the report counts as a final safety net before the commit lands."

Do NOT run `git add` or `git commit` from this skill. Leave staging and commit message authoring to `/commit`, which has the proper component grouping, CHANGELOG handling, and the redundant ConformU validation gate.

## Workflow notes

- This skill assumes Linux arm64 only. AlpacaBridge does not support amd64; do not save reports under any other platform name.
- If the run is interrupted (Ctrl-C, network drop, AlpacaBridge crash), discard the partial logs in `$TMPDIR` and start over after fixing the cause.
- Never edit a saved ConformU log by hand to "fix" timing or counts — that is the worst possible thing to do to the validation record. If a log is wrong, delete it and re-run.
