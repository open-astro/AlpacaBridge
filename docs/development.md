# Development Guide

AlpacaBridge is developed **Claude-first**: the project ships with [Claude Code](https://claude.com/claude-code) skills that automate the entire driver lifecycle — from scaffolding a new vendor driver through ConformU hardware validation to opening the pull request — and an [AGENTS.md](../AGENTS.md) knowledge base that captures every architecture rule and hardware lesson learned. Start there; the manual build, test, and packaging reference follows for the full picture of how everything works.

For end-user install instructions, see the [README](../README.md).

## Supported platforms

- **Debian 13 (Trixie)** on `arm64` (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate) — the only supported architecture
- Linux arm64 only — no amd64/x86_64, no 32-bit, no Windows or macOS

### Development machines

You don't need an SBC to develop — any arm64 machine running a **Debian 13 arm64 VM** makes a fast dev box:

- **Apple Silicon Mac** (M1 or newer) — run Debian 13 arm64 in [UTM](https://mac.getutm.app/), Parallels, or VMware Fusion
- **Windows on ARM laptop** (Snapdragon X, etc.) — run Debian 13 arm64 in Hyper-V or VMware Workstation

Inside the VM the full toolchain, test suite, and CI pre-flight run natively at full speed. USB devices can be passed through to the VM for hardware work, but final ConformU validation should still run on a real supported SBC (see the [`/conformu` skill](#conformu--hardware-validation)).

## Developing with Claude Code

Open Claude Code in the repository root and the skills below are available as slash commands (defined in `.claude/commands/`). Together they cover the whole workflow:

```
/driver-build  →  /deploy-test  →  /conformu  →  /commit  →  /submit-pr
   implement       deploy to SBC     validate      commit       PR + review
```

### `/driver-build` — guided driver implementation

Walks through building, extending, or fixing device drivers:

- Verifies the vendored ASCOM Alpaca API spec is current against ascom-standards.org before any code is written
- Interactive Q&A: device type, vendor, connection method, SDK availability
- Searches INDI/INDIGO for reference drivers with fuzzy vendor name matching
- Creates the feature branch (`driver/<vendor>-<device>`)
- Guides through the 3-layer architecture, SDK cleanup, auto-detection, CMake setup, and AlpacaHTTP/web UI integration
- Enforces Catch2 tests (8 cases, 30+ assertions) and ASCOM Alpaca API compliance
- Validates with ConformU on arm64 and updates AGENTS.md with lessons learned

### `/deploy-test` — push the build to a test SBC

Gets the current working tree running on the test device so ConformU validates the right build:

- Builds the .deb from the working tree (`scripts/build_deb.sh`), reporting exactly what is deployed (branch, commit, dirty-tree state)
- Copies it to the SBC over SSH, installs it, and restarts `alpacabridge.service`
- Verifies the device is reachable and reports the deployed version via the management API before handing off to `/conformu`

### `/deploy-remote-test` — same, for an SBC with no SSH path

For a test SBC reachable only through a Raspberry Pi Connect browser shell (no LAN/SSH):

- Builds the .deb and publishes it as a temporary GitHub pre-release (`test-<suffix>-<hash>` tag) on the project repo
- Prints the paste-ready `curl` / `dpkg -i` / restart / md5 verification commands for the remote shell
- Deletes the pre-release and its tag once testing is done; ConformU then runs on the SBC against `127.0.0.1:6800`

### `/conformu` — hardware validation

Runs ConformU against a connected AlpacaBridge device and processes the results:

- Checks the installed ConformU against the [latest upstream release](https://github.com/ASCOMInitiative/ConformU/tags) and offers to update before running. On arm64 an installed 4.5.0 or 4.5.1 is always treated as outdated (4.5.0 has the [ConformU 4.5.0 arm64 timing bug](../SUPPORTED-DRIVERS.md); the standard is 4.5.2), and the replacement is the sha256-checked 4.5.2 GitHub release asset
- Validates the full pass criteria: 0 errors, 0 issues, **and** 0 timing issues (the Timing Summary is a separate pass criterion)
- Saves the logs under `AlpacaCore/conformu/<vendor>/<model>/`
- Updates [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) with the validated entry

### `/commit` — stage, review, and commit

- Assesses the working tree, reviews diffs, flags red flags (SDK bloat, secrets, build artifacts)
- Hard-blocks committing failing ConformU reports
- Updates SUPPORTED-DRIVERS.md and `docs/architecture.md` if driver or ConformU changes are present
- Adds the PR's changelog fragment (`changelog.d/<branch-slug>.md`, never `CHANGELOG.md`); the release derives the SemVer bump from it (new driver = minor, fix/docs = patch, breaking = major)
- Writes verb-first commit messages with vendor/device specificity

### `/submit-pr` — pull request submission

- Safety checks: refuses to PR from `main`, blocks on uncommitted changes and failing ConformU reports
- Auto-detects direct contributor vs fork and handles both flows
- Runs the pre-submission checklist (tests, ConformU, changelog fragment, SUPPORTED-DRIVERS, AGENTS.md, license headers, SDK cleanup)
- Reproduces CI locally via `scripts/ci_preflight.sh` before pushing, so PRs never open red
- Builds the PR title and a body that follows `.github/PULL_REQUEST_TEMPLATE.md` (thinking path, linked issues, what changed, verification, risks, model used, checklist), then creates the PR via `gh`; the `pr-template` job in `.github/workflows/pr-body.yml` (`scripts/check_pr_template.py`) fails a PR whose body drops a template section or leaves one unfilled
- Watches for the automated review verdict and batches fixes into single pushes (every push restarts a full fresh review)

### AGENTS.md — the knowledge base

[AGENTS.md](../AGENTS.md) is the project's living memory: architecture rules, the three-layer driver pattern, ConformU failure patterns, and per-vendor quirks discovered on real hardware. Every driver session ends by updating it — if something surprising was learned, it gets documented so the next session (human or agent) doesn't rediscover it. Read it before touching any driver code.

## Prerequisites

```sh
sudo apt install git build-essential cmake g++ \
    libusb-1.0-0-dev libudev-dev libgpiod-dev \
    libhidapi-dev \
    libltdl-dev libexif-dev libjpeg-dev libraw-dev \
    nlohmann-json3-dev libcurl4-openssl-dev \
    zlib1g-dev libsystemd-dev pkgconf \
    catch2
```

This matches the `Build-Depends` field of `debian/control`, the authoritative list, plus `git` and `catch2` for the tests.

libgphoto2 is not taken from apt. A build with `ALPACACORE_ENABLE_GPHOTO=ON` needs the bundled copy first: run `sudo scripts/build_bundled_gphoto.sh` once (it builds the vendored tarball into `/usr/lib/alpacabridge/libgphoto2`; `build_and_run.sh` and `install_alpaca_service.sh` do this for you when the installed copy is missing or at another version), or build into a scratch tree with `scripts/build_bundled_gphoto.sh --destdir DIR` and configure with `-DALPACACORE_GPHOTO_STAGE=DIR`. CMake stops with that instruction when the bundle is missing. On a host with the `.deb` installed, the first `build_and_run.sh` or `install_alpaca_service.sh` run rebuilds the bundle over the package's files, because the package ships no `pkgconfig/` file to read the version from; the rebuild writes that file, so later runs skip it. The gphoto link passes `--disable-new-dtags`, so an executable that links the gphoto driver (the server among them) gets its whole rpath as a `DT_RPATH`, which the loader searches before `LD_LIBRARY_PATH`: in a vendors-ON dev build, `LD_LIBRARY_PATH` cannot override a library found on that rpath.

Verify: `cmake --version` (3.20+), `g++ --version` (GCC 10+, C++20 required).

> `libgpiod-dev` (>= 2.0) is required only for the GPIO-backed power-port Switch drivers (ZWO ASIAIR, iOptron iMate, ToupTek StellaVita). Without it those switches are cleanly disabled at configure time and the rest of the build is unaffected. See [PowerPorts.md](../AlpacaCore/PowerPorts.md).
> `libhidapi-dev` is required for the Astroasis USB HID focuser driver when building with all vendors enabled.

## Build and run

```sh
chmod +x build_and_run.sh
./build_and_run.sh
```

The server starts on port **6800**: `http://localhost:6800/`

### Manual build

```sh
cmake -S AlpacaHTTP -B AlpacaHTTP/build -DALPACACORE_ENABLE_ALL_VENDORS=ON
cmake --build AlpacaHTTP/build --parallel
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `ALPACACORE_BUILD_TESTS` | `ON` | Build unit tests (requires Catch2) |
| `ALPACACORE_ENABLE_ALL_VENDORS` | `ON` | Enable all implemented vendor drivers |
| `ALPACACORE_ENABLE_ZWO` | `OFF` | ZWO cameras, focusers, rotators, switches, filter wheels |
| `ALPACACORE_ENABLE_QHY` | `OFF` | QHY cameras |
| `ALPACACORE_ENABLE_IOPTRON` | `OFF` | iOptron mounts |
| `ALPACACORE_ENABLE_SYNSCAN` | `OFF` | SynScan mounts |
| `ALPACACORE_ENABLE_CELESTRON` | `OFF` | Celestron mounts |
| `ALPACACORE_ENABLE_SVBONY` | `OFF` | SVBONY cameras |
| `ALPACACORE_ENABLE_TOUPTEK` | `OFF` | ToupTek cameras |
| `ALPACACORE_ENABLE_ALTAIR` | `OFF` | Altair Astro cameras (ToupTek OEM; requires `ALPACACORE_ENABLE_TOUPTEK`) |
| `ALPACACORE_ENABLE_PLAYERONE` | `OFF` | Player One cameras |
| `ALPACACORE_ENABLE_GEMINI` | `OFF` | Losmandy Gemini focusers |
| `ALPACACORE_ENABLE_WEEWX` | `OFF` | WeeWX observing conditions |
| `ALPACACORE_ENABLE_ONSTEP` | `OFF` | OnStep mounts |
| `ALPACACORE_ENABLE_SKYWATCHER` | `OFF` | SkyWatcher motor controller mounts |
| `ALPACACORE_ENABLE_BISQUE` | `OFF` | Bisque/Paramount (TheSkyX) telescope support |
| `ALPACACORE_ENABLE_WANDERERASTRO` | `OFF` | WandererAstro CoverCalibrator |
| `ALPACACORE_ENABLE_ASTROASIS` | `OFF` | Astroasis Oasis Focuser |
| `ALPACACORE_ENABLE_GPHOTO` | `OFF` | libgphoto2 DSLR/mirrorless cameras (Canon, Nikon, Sony) |

## Running tests

```sh
./run_all_tests.sh
```

The build runs at `nproc`, but `ctest` runs at twice that: most of the suite
waits on fake hardware in real time rather than computing, so tying it to the
core count leaves the machine idle. Override with `CTEST_PARALLEL` on a machine
where that is too aggressive:

```sh
CTEST_PARALLEL=4 ./run_all_tests.sh
```

`run_all_tests.sh` also guards against a shrunken suite. `ctest --no-tests=error` fails only a
run that finds zero tests, so a vendor target that silently stops configuring would still pass
with most of its cases gone. After the build the script counts the tests with `ctest -N`
(AlpacaCore in its subdirectory, AlpacaHTTP as the rest of the tree) and fails, naming the
configuration, the count found and the floor, when AlpacaCore or AlpacaHTTP finds fewer than its
floor. The floors sit at the top of `run_all_tests.sh`, keyed by `ALPACACORE_ENABLE_ALL_VENDORS`
(`OFF` or `ON`), about 10% below the counts at the time they were set; any other value has no
floor and fails. When tests are removed on purpose, lower the floor in the same PR; when the
suite grows a lot, raise it so the guard stays tight.

`run_all_tests.sh` builds both suites in one tree, `AlpacaHTTP/build` (issue #1251): AlpacaHTTP
adds AlpacaCore as a subdirectory, so the core tests land in `AlpacaHTTP/build/AlpacaCore/tests`
and every AlpacaCore source is compiled once. Or manually, from the repository root:

```sh
ctest --test-dir AlpacaHTTP/build              # both suites
ctest --test-dir AlpacaHTTP/build/AlpacaCore   # AlpacaCore only
```

Filter by tag: `AlpacaHTTP/build/AlpacaCore/tests/alpacacore_tests [zwo][camera]`
Exclude hardware tests: `AlpacaHTTP/build/AlpacaCore/tests/alpacacore_tests ~[hardware]`

These assume the tree `run_all_tests.sh` just made. A pre-flight leaves a different
one there (see below), so after one, rebuild rather than filtering against what is left.

Before pushing, reproduce the full CI gate set locally:

```sh
./scripts/ci_preflight.sh
```

This runs clang-format, the Unicode/Trojan-Source scan, both build+test configurations (vendors OFF and ON), clang-tidy, cppcheck, and — when the relevant files changed — shellcheck, the web UI JavaScript gate (`node --check` for syntax plus `node --test` for the pure formatters in `AlpacaHTTP/web/format.js`, triggered by changes under `AlpacaHTTP/web/` or `AlpacaHTTP/tests/web/`), and zizmor. It finishes with the ASan+UBSan pass, which is on by default since #588 and runs last of the default gates, after zizmor; `RUN_SANITIZERS=0` opts out, and the opt-in `RUN_TSAN=1` and `RUN_SCAN_BUILD=1` passes run after it when set. `/submit-pr` runs it automatically.

## Writing tests

Every driver requires at minimum **8 test cases** and **30+ assertions** using Catch2. Tests live in `AlpacaCore/tests/`.

### Required test cases

1. **Device information** — device type, name, unique ID, description, driver version, interface version
2. **Connection states** — initial disconnected, connect/disconnect cycle
3. **Not-connected errors** — every operation while disconnected must throw `AlpacaException` with `AlpacaError::NotConnected` error code
4. **Capability reporting** — all capability properties return valid values without throwing
5. **Device-specific operations** — exposure for cameras, slewing for telescopes, etc.

### ASCOM contract tests (ConformU alignment)

These tests catch the bugs that fail ConformU. They run without hardware and verify that the driver follows the ASCOM Alpaca specification at the error code and state machine level.

All test files use the `require_alpaca_error` helper to verify both the exception type and the specific error code:

```cpp
#include <alpacacore/util/error_handling.h>
#include <functional>

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

} // namespace
```

6. **ASCOM error codes** — verify the correct Alpaca error code, not just that it throws. ConformU checks error numbers, not exception messages.

```cpp
TEST_CASE("Vendor Device - ASCOM Error Codes", "[vendor][device][unit]") {
    auto driver = create_vendor_device(0, 0);

    // Not connected must return NotConnected (0x407), not generic DriverException
    require_alpaca_error([&]() { driver->get_right_ascension(); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_declination(); },
                         alpacacore::AlpacaError::NotConnected);

    // Invalid values must return InvalidValue (0x401)
    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); },
                         alpacacore::AlpacaError::InvalidValue);
}
```

7. **Value range validation** — invalid inputs must throw `InvalidValue` (0x401), not silently normalize or throw a generic error. Valid values must persist (set then get back).

```cpp
TEST_CASE("Vendor Telescope - Target Coordinate Persistence", "[vendor][telescope][unit]") {
    auto driver = create_vendor_telescope(0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);

    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    // Changing one target must not affect the other
    REQUIRE_NOTHROW(driver->set_target_right_ascension(6.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 6.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);
}
```

8. **State machine contracts** — verify that device state follows ASCOM rules without needing hardware.

```cpp
TEST_CASE("Vendor Camera - State Machine Contracts", "[vendor][camera][unit]") {
    auto driver = create_vendor_camera(0, 0);

    require_alpaca_error([&] { (void)driver->get_camera_state(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_is_pulse_guiding() == false);
    REQUIRE(driver->get_can_abort_exposure() == true);
    REQUIRE(driver->get_can_stop_exposure() == true);
}
```

### What's testable without hardware

Most ConformU checks can be replicated without hardware. The key is knowing which driver operations require a live connection and which don't.

**Testable without hardware (all device types):**
- All error codes (NotConnected, InvalidValue, NotImplemented)
- Capability flags (CanSlew, CanAbortExposure, etc.)
- Device metadata (Name, Description, InterfaceVersion)
- State machine initial state (Idle, not moving, not guiding)
- Unsupported action/method error codes

**Testable without hardware (telescope-specific):**
- Target coordinate range validation and persistence
- Site elevation validation ([-300, 10000] meters)
- Site latitude/longitude range validation
- Alignment mode and equatorial system values
- Tracking rates (non-empty list)
- Axis rate ranges
- Slew settle time validation

**Requires hardware:**
- Actual slew/move/exposure operations
- Position readback (RA, Dec, altitude, azimuth)
- Temperature readings
- Tracking state changes
- Image data capture
- Setting site latitude/longitude values (some drivers require connection)

### Device-type specific contracts

**Camera drivers** must also test:
- `CameraState` and `PercentCompleted` throw `NotConnected` while disconnected, and `CameraState` is `Idle` when connected before the first exposure
- `IsPulseGuiding` is false when idle
- `ImageReady` is false before any exposure (or throws NotConnected)
- `CanAbortExposure` and `CanStopExposure` are reportable

**Telescope drivers** must also test:
- Target RA/Dec range validation with `InvalidValue` error codes
- Target coordinate persistence (set, read back, verify independence)
- Site elevation range [-300, 10000] with `InvalidValue` for out-of-range
- Site latitude/longitude range validation
- `EquatorialSystem` returns a valid value
- `AlignmentMode` returns a valid value
- `TrackingRates` returns a non-empty list
- `SlewSettleTime` is non-negative, negative values throw `InvalidValue`
- Axis rate ranges for all three axes (tertiary should be empty)

**Focuser drivers** must also test:
- `Absolute` and `TempCompAvailable` are reportable
- `IsMoving` is false when idle
- All disconnected operations return `NotConnected` error code

Use `SKIP("reason")` when hardware isn't available. Tag hardware tests with `[hardware]`.

## Building drivers

AlpacaBridge drivers follow a **three-layer architecture**:

1. **Pure virtual interface** (`include/alpacacore/<device>_driver.h`) — already exists for all device types
2. **SDK/Protocol wrapper** (`include/alpacacore/vendor/<vendor>/`) — clean C++ interface wrapping the vendor SDK or serial protocol
3. **Vendor implementation** (`src/vendors/<vendor>/`) — concrete driver using the wrapper

SDK-based drivers (ZWO, QHY, Player One, SVBONY, ToupTek) use an **SDK wrapper**. Protocol-based drivers (iOptron, SynScan, Celestron, Gemini) use a **protocol wrapper**.

Use the [`/driver-build` skill](#driver-build--guided-driver-implementation) for the full interactive workflow — it walks through every step from SDK placement through ConformU validation. See [AGENTS.md](../AGENTS.md) for architecture rules and the [instruction index](agent-instructions.md) for vendor-specific lessons learned.

## Installing a source build as a service

```sh
./install_alpaca_service.sh install    # build + install systemd service
./install_alpaca_service.sh update     # rebuild + restart
./install_alpaca_service.sh uninstall  # remove service
./install_alpaca_service.sh status     # show status
```

For production use, prefer the apt package from [apt.openastro.net](https://apt.openastro.net).

## Dependency graph

GitHub's dependency graph parses the workflow action pins itself, but stops listing them for a workflow file a submitted snapshot names (seen on 2026-10-16 for `ci.yml`, `osv-scan.yml`, `nightly-build.yml` and `release.yml`; the pins came back once the snapshot listed them), so the snapshot lists those workflows' `uses:` pins itself (`pkg:githubactions`, development). `.github/workflows/dependency-submission.yml` also submits, on every push to `main`, a snapshot built by `scripts/dependency_snapshot.py` from `debian/control` (Build-Depends as development, Depends and Recommends as runtime; the first package of an `a | b` alternative; the version from an exact `(= X)` pin, else the candidate `apt-cache policy` reports in the `debian:trixie` image at submission time, so the graph shows what a build installs that day and moves with Debian point releases), the CMake `FetchContent_Declare` entries in `AlpacaHTTP/CMakeLists.txt` (`pkg:github`), the vendored SDKs listed in `scripts/dependency_sdk_versions.json` (`pkg:generic`; `version` is `null` where the tree holds no version), the system packages the installed daemon needs that `Depends` does not name (`RUNTIME_SYSTEM` in the script: systemd, dbus, udev, apt, dpkg, bash, dash and coreutils, each with the file and line pattern that proves the need; versioned from the same `debian:trixie` candidates), and, as development dependencies, the tools `.github/workflows/ci.yml` installs outside `debian/control`: cppcheck and zizmor at the `ver=` pin in their install steps (`pkg:github`; OSV-Scanner's pin in `.github/workflows/osv-scan.yml` is read the same way, from `WORKFLOW_TOOL_STEPS`), and the tools CI installs from the runner's Ubuntu 24.04 apt at the runner's `apt-cache policy` candidate (Catch2, clang-format, clang-tidy, gcovr, nodejs, shellcheck; `CI_APT_TOOLS` in the script); and the ConformU build `.claude/commands/conformu.md` pins for rig validation (`pkg:github/ASCOMInitiative/ConformU@4.5.2-beta`, the maintainer's beta installed from the project's `tools/` mirror, since 4.5.2 is not a GitHub release yet); and every job `container:` image in `.github/workflows/` (`pkg:docker`, e.g. the `debian:trixie` image `release.yml` and `nightly-build.yml` build the published `.deb` in). `--check` fails if a `ver=` pin, an apt tool's install line, the ConformU build line or a `RUNTIME_SYSTEM` evidence pattern can no longer be found, so a reworded step cannot drop an entry silently. The `deb` and `generic` entries document what ships and raise no Dependabot alerts. `${shlibs:Depends}` is resolved only at package build time, so `dependency-submission.yml` downloads the `nightly-build` `.deb` on every push, picks the newest by its `nightlyYYYYMMDDHHMM` stamp (`--newest-deb`, never by full file name, which sorts `5.0.10` before `5.0.9`; a file without a stamp is ignored, and when `.deb` files exist but none has a stamp it exits 1, so the fetch step fails) and passes it with `--deb`: the snapshot then also reports the built package's `Depends` (shlibs resolved, so every library the binary and the vendored `.so` files link) and `Recommends` as runtime, versioned from the same `debian:trixie` candidates, in a manifest whose `source_location` is `debian/rules`. The nightly is built from `main` once a day, so these entries can trail `main` by a day; with no nightly to read (a fork) they are left out with a warning. They are not a separate snapshot because one the nightly submitted against its own commit dropped out of the graph as soon as `main` moved (#1239), and their `source_location` is not `debian/control` because two manifests on one location may replace each other. The `dash` and `coreutils` entries in `RUNTIME_SYSTEM` take their evidence only from the vendored udev rules files `debian/rules` installs (its `:=` and `=` variables expanded; `--check` fails when a `.rules` path uses a `?=`, `+=` or `$(shell ...)` variable or keeps an unresolved `$(VAR)` in a path whose text before the variable is the vendored tree or one of its ancestor directories, instead of skipping it; an unresolved variable after a prefix that cannot lead into it, such as the libgphoto2 stage path, is not vendor evidence), not from every `.rules` file under `AlpacaCore/external/`.

When you add, remove or bump a directory under `AlpacaCore/external/`, edit `scripts/dependency_sdk_versions.json`: an SDK or library goes in `sdks` (path, name, version, where the version came from), a directory of documents or udev rules only goes in `doc_only`. The `docs-drift` CI job runs `python3 scripts/dependency_snapshot.py --check` and names the path to add or remove. Run `python3 scripts/dependency_snapshot.py` to print the snapshot; the VM never submits it.

`python3 scripts/dependency_snapshot.py --spdx <file.deb>` writes the same inventory as an SPDX 2.3 SBOM rooted at that built package, with its SHA-256: runtime dependencies come from the package's own `Depends` field (so `${shlibs:Depends}` is resolved there), the vendored SDKs and FetchContent libraries are `CONTAINS`, and Build-Depends are `BUILD_DEPENDENCY_OF`. Runtime and build dependencies carry the version installed in the build environment (`dpkg-query`), so the SBOM records which library versions the `.deb` was built against; a package not installed there stays unversioned. `release.yml` (beta) and `nightly-build.yml` attest that SBOM against the `.deb` they publish, next to its build provenance.

Each `sdks` entry also carries a `sha256` pin: a digest of the git-tracked files under its `path`. After you change an SDK's files, check its `version` and run `python3 scripts/dependency_snapshot.py --pin`; `--check` fails until the pins match. `--spdx` unpacks the `.deb` and requires every file under `/usr/lib/alpacabridge/` and `/usr/lib/firmware/` to be byte-identical to a git-tracked vendored file of a listed SDK (the package skips `dh_strip` and `dh_dwz` for `/usr/lib/alpacabridge/` for this reason); it lists each one in the SBOM with its SHA-1 and SHA-256 under its SDK, and stops with an error naming any file that matches none, except the libgphoto2 bundle under `/usr/lib/alpacabridge/libgphoto2/`, which `debian/rules` builds from the vendored tarball (`BUILT_FROM_VENDORED_SOURCE` in the script; the tarball's SDK entry is still `CONTAINS`-ed). Build-Depends carry the exact version `dpkg-query` reports in the build environment.

## Dependency update checks

`.github/workflows/dependency-updates.yml` runs `scripts/dependency_updates.py` every Monday. Nothing it tracks is a manual check. It reads three sources of entries:

- every `pkg:github` pin in the dependency snapshot (cppcheck, zizmor and OSV-Scanner at the `ver=` pins in their workflow steps, nlohmann/json, ConformU), against the repository's latest GitHub release, or its highest version tag when it publishes no releases;
- every SDK in `scripts/dependency_sdk_versions.json`, through its `updates` block: `release-page` (the highest version a regex matches on a page: libgpiod, libgphoto2 (the GitHub tags page), QHY, SVBONY, ToupTek, Player One) or `json` (a field of the matching entry in a JSON list: ZWO's SDK endpoint);
- the `documents` list in the same file: the protocol documents and reference sources the drivers are built from, each with a reviewed `baseline`. `github-file` compares the newest commit on an INDI driver file a driver was derived from; `hash` compares the sha256 of a PDF, of the ConformU beta tarball (an unpinned URL that can be replaced in place), or of an HTML page's visible text (scripts, comments and whitespace removed, so the hash only moves when the content does); the ASCOM Alpaca Device and Management API schemas and the Alpaca API Reference page are watched this way too, the schemas by the raw bytes ascom-standards.org serves, CRLF line ends included, so their `baseline` is not the LF-normalized snapshot hash the `ascom-alpaca-protocol` skill pins; `page` compares the highest version a regex matches (Wanderer's dated protocol PDFs, the vendor ASCOM installers the Gemini and Oasis protocols were recovered from). An entry may carry a `note`, printed in the issue the weekly check opens for it: the ConformU beta's note says to adopt and mirror the new beta through `/conformu` before moving the baseline, and to keep the URL on the ASCOM beta download.

`python3 scripts/dependency_snapshot.py --check` (the `docs-drift` CI job) keeps this coverage complete as the tree grows. It fails when:

1. an SDK in `sdks` has no `updates` block;
2. a docs-only directory has neither a `documents` entry whose `path` is under it nor a `no_upstream` entry giving the reason (for example the ASIAIR Plus udev rules, written in this repository);
3. a directory under `AlpacaCore/src/vendors/` is not in `vendors`, which maps each vendor to the SDK and document names it is built from (`tracked`) or to a `no_upstream` reason (weewx: a feed the user configures), or `vendors` names an SDK, document or directory that does not exist;
4. a workflow step pins a tool with `ver=` that `CI_TOOL_STEPS` or `WORKFLOW_TOOL_STEPS` does not list.
5. a `system_libraries` entry has no upstream `repo` or no `baseline`.

So a new vendor, SDK, protocol folder or pinned tool cannot merge without an update check, or a stated reason why it has none. Each failure names the file and the entry to add.

Debian, Ubuntu and docker entries are not checked for updates: apt and the image tag already resolve to the newest package when CI builds. The libusb copy bundled in `AlpacaCore/external/SVBONY/lib/armv8/` is not tracked either: `debian/rules` packages only `libSVBCameraSDK.so*`, so that copy is neither shipped nor linked, and the system libusb is used at runtime. The exception is `system_libraries`: libraries a driver links that the build takes from Debian (LibRaw for the gphoto camera driver; libgphoto2 is bundled and tracked as an SDK entry), each with its upstream GitHub repository and the version Debian trixie ships as `baseline`. An upstream release newer than the baseline opens an update issue that says there is no pin to bump: review what the release adds for the driver (new camera bodies, fixes), then move the baseline. The gphoto2 command-line tool, whose bulb sequence the gphoto driver follows, is a `documents` entry with the `github-release` provider.

Every update or document issue ends with a `## Changelog` section when the source has one: the GitHub release notes (quoted in a code block, so an `@mention` or `#123` in them pings no one), a compare link for a tag-only release or a changed INDI file, ZWO's per-SDK update log (`changelog_field`), or a static `changelog` URL from the table (QHY, libgpiod). A changelog that cannot be fetched only drops the section; the issue still opens.

Each newer version opens one issue, "Update <name> <current> -> <latest>" (`dependencies`, `enhancement`, `P4`); a changed document opens "Upstream document changed: <name> <baseline> -> <upstream>" (`dependencies`, `documentation`, `P4`), which closes once a pull request sets its `baseline` to the upstream value after reviewing the change against the driver. A still newer release retitles that issue with a comment instead of opening a second one, and the issue closes itself on the first run that finds the repository at that version. Closing an update issue by hand means "not this version": the check opens no new issue for it until upstream releases a later version or the repository moves to a different version (for example a `Closes #N` pull request that bumped only part of the way). Closing a broken-source issue by hand means "known": no new copy opens while the source stays unreadable, and once it reads again a later break is reported as usual. An open issue for a dependency that leaves the inventory closes on the next run. If an issue call fails (a missing label, a rate limit), the run still handles the other dependencies, lists the failure and exits non-zero. Each page or API request times out after 30 s and a hashed document after 300 s; a timeout, a refused or dropped connection or a corrupt body is a broken source for that entry, never a crash of the run. Issues are found by their `dependencies` label: removing that label from one makes the next run open a duplicate. A large OSV result lists at most 100 findings per summary issue, so the body stays under GitHub's size limit; the rest open the next day's issue. A source that cannot be read (a page that changed, a repository that moved) opens "Update check broken: <name>" (`dependencies`, `bug`, `P4`), so a broken checker is never mistaken for "no update"; it closes when the source reads again. Each issue keeps its state in a `<!-- dependency-updates: {...} -->` comment in its body, so the workflow never commits. Run `GH_REPO=open-astro/AlpacaBridge python3 scripts/dependency_updates.py --dry-run` to print the table and the issue changes without making them (set `GH_TOKEN`: without one, the GitHub commit and tag lookups run into the unauthenticated rate limit); the `docs-drift` CI job runs its `--self-test`.

`.github/workflows/osv-scan.yml` runs daily after the nightly build. It installs the nightly `.deb`'s `Depends` into a `debian:trixie` image with `apt-get satisfy`, so every runtime package is at the exact version a user gets, and scans that image with OSV-Scanner, which reads Debian advisories. (The SBOM is not scanned directly: its runtime versions are those of the build environment, not the versions a user installs, and OSV matches exact installed versions.) The workflow then keeps only what AlpacaBridge can act on: findings in the source packages of the `.deb`'s own runtime dependencies (its `Depends`, mapped to Debian source packages with `dpkg-query` in the scan image), without the C/C++ runtime (glibc, gcc-N: every program links it and no application can replace it) and without anything Debian rates `unimportant`. Only direct dependencies count: a library one of them pulls in (OpenSSL or nghttp2 under libcurl, for example) is outside this scope even though the binary loads it. Of those, a finding goes to Security > Code scanning (category `osv-scanner-nightly-deb`) only when it has a `fixed` version in its Debian trixie entry, or when it is Critical (CVSS 9.0 or more) with no fix, since that may call for dropping the dependency rather than waiting for Debian; an unfixed lower-severity finding is left out until a fix lands (code scanning then closes its alert as fixed, and the same fingerprint reopens it the day Debian ships one). The base trixie system's findings (on the first scan, 255 of 298, none with a trixie fix) never reach code scanning. The full unfiltered result (`osv-all.sarif`, `osv-all.json`) is kept for 14 days as each run's `osv-full-results` artifact. If no SARIF result matches the message format the filter reads, or the JSON names packages by binary rather than Debian source name, the step fails instead of reporting an empty result; the artifact is uploaded before both checks. A finding also opens an issue (`dependencies`, `bug`, `P2`, one per scan, listing every new finding with the version that fixes it) only when it is actionable: CVSS 7.0 or more, a `fixed` version in the advisory's Debian trixie (`Debian:13`) entry, and not rated `unimportant` by Debian. One advisory reported through several binary packages of the same source (glibc as `libc6`, `libc-bin`, ...) is one finding. Lower and unscored findings with a trixie fix stay in code scanning only; unfixed non-Critical findings, `unimportant` ones and the base system's are only in the artifact: the base trixie system alone carries hundreds, and a fix for them reaches users through Debian, not through this project. Summary issues carry an `osv2` marker; the first format (`osv`, #1135, which listed unfixable findings) is ignored. The vendored SDKs have no advisory source and are not covered.

## Actions cache pruning

The compiler caches in `ci.yml` and `nightly-build.yml`, and CodeQL's overlay database, are saved under a key that ends in the commit SHA and restored by prefix, which picks the newest cache with that prefix. Only that newest cache can ever be restored, so `.github/workflows/cache-prune.yml` runs after each CI, Nightly Build and CodeQL run on `main` and deletes every cache that a newer one with the same ref and key prefix has superseded (`scripts/prune_actions_caches.py`, issue #1350). Without it every push added about 2 GB, the repository passed GitHub's 10 GB cache limit and GitHub evicted caches still in use. The prefix is the key up to its 40-hex commit SHA, so each job's prefix is pruned on its own, even one that begins another job's prefix; a key without a commit SHA (the cppcheck build) is never deleted. To see what a run would delete: `python3 scripts/prune_actions_caches.py --repo open-astro/AlpacaBridge --dry-run`.

## Packaging

The Debian package is built from the `debian/` directory. It installs:

- Binary at `/usr/bin/alpacabridge`
- Vendor libraries under `/usr/lib/alpacabridge`, and libgphoto2 with its camlibs under `/usr/lib/alpacabridge/libgphoto2` (built from the vendored tarball; the executables find it through an rpath, so the host's own libgphoto2 packages are untouched and still used by other programs, and AlpacaBridge no longer depends on them; [decision 0012](decisions/0012-bundled-libgphoto2.md))
- Configuration under `/etc/alpacabridge`
- Systemd unit `alpacabridge.service` running as the `alpacabridge` system user
- The software-update helper: root-owned oneshot unit `alpacabridge-update.service`, its script `/usr/libexec/alpacabridge/software-update`, and the polkit rule `/usr/share/polkit-1/rules.d/50-alpacabridge-update.rules` that lets the service user start that one unit and the two `alpacabridge-channel@stable`/`@beta` update-channel units (helper `/usr/libexec/alpacabridge/set-update-channel`; see [software-update.md](software-update.md)). `debian/rules` passes only `alpacabridge.service` to `dh_installsystemd` so the helper never gets enable/start/restart snippets.

## Scheduled run failures

`.github/workflows/scheduled-failure.yml` runs after each scheduled or manually dispatched run of Nightly Build, CodeQL, OSV scan and Dependency updates on `main` (issue #1352). A failed, timed-out or never-started run opens one issue for its workflow (`Scheduled run failing: <workflow>`, labels `bug` and `P3`), or adds a comment with the run link when that issue is already open; the next successful run of that workflow closes it, unless one of that run's jobs was skipped (a Nightly Build run whose `check` job found the tag already at `HEAD` skips the build and the suite, so it does not show that the failure is fixed). A cancelled run changes nothing. The issue is found by the `<!-- scheduled-run: ... -->` marker in its body, so it can be retitled or relabelled. The rules are in `scripts/scheduled_run_issue.py`; to see what it would do for a run: `WORKFLOW_NAME="Nightly Build" CONCLUSION=failure RUN_URL=<url> HEAD_SHA=<sha> EVENT=schedule GH_REPO=open-astro/AlpacaBridge python3 scripts/scheduled_run_issue.py --dry-run`.

## Releases

The only install channel is the OpenAstro APT repository ([apt.openastro.net](https://apt.openastro.net)): install once, then `apt upgrade`. Every version published there is also marked in this repository so a shipped build has a name:

- A git tag `vX.Y.Z` on the merge commit that carried the release (the `VERSION` file, the README badge, and the dated CHANGELOG heading all agree at that commit).
- A GitHub Release for that tag, created automatically by `.github/workflows/release.yml`. Its notes are the plain-language `docs/releases/X.Y.Z.md` (falling back to the version's CHANGELOG section). Every Release, stable or beta, also carries the arm64 `.deb` and the `alpacabridge-dbgsym` debug-symbols package, each with a `.sha256`, built by the same workflow in a Debian trixie container and named with the tag spelling: `alpacabridge_X.Y.Z_arm64.deb` for a stable release, `alpacabridge_X.Y.Z-beta.N_arm64.deb` for a beta pre-release (`vX.Y.Z-beta.N`), so testers can install a named beta with `sudo apt install ./alpacabridge_X.Y.Z-beta.N_arm64.deb` ([beta channel](beta-channel.md)). The apt repository re-hosts these assets by name, so `release.yml` keeps the names fixed. The `.deb`, like each nightly's, also gets a signed build provenance attestation and an SPDX SBOM attestation, listed under the repository's Actions > Attestations.

To cut a release or a beta, run `/bump-release` (Claude Code skill, `.claude/commands/bump-release.md`). It does the whole flow: writes `VERSION`, updates the README badge and device count, assembles the changelog fragments into a dated CHANGELOG section, writes plain-language notes to `docs/releases/X.Y.Z.md`, opens and merges the release PR, tags the merge commit, and verifies the Release. By hand the same steps are:

1. On a `release/X.Y.Z` branch: write `VERSION`, update the README badge line, run `python3 scripts/changelog_fragments.py --release X.Y.Z --date <today>` (it writes the dated CHANGELOG section and deletes the `changelog.d/` fragments), and write `docs/releases/X.Y.Z.md` for the people who will not read the CHANGELOG (what changed, what to do, no issue numbers or code names).
2. Merge the PR.
3. Tag the merge commit and push the tag:

```bash
git checkout main && git pull
git tag -a vX.Y.Z -m "Release X.Y.Z"
git push origin vX.Y.Z
```

The Release body is `docs/releases/X.Y.Z.md` with a link to the CHANGELOG section appended; when no notes file exists the CHANGELOG section itself is used. The workflow refuses a tag whose version does not match `VERSION`, or whose CHANGELOG section is missing or still `UNRELEASED`, so a tag can never publish notes for an uncut release. Preview the CHANGELOG notes locally with `scripts/changelog_section.py X.Y.Z`.

Betas and hotfixes follow the stable-branch flow in [beta-channel.md](beta-channel.md): `/bump-release` cuts from `main` only to create `stable/X.Y` and its first beta; every later beta, the stable release and hotfixes are cut on `stable/X.Y`, and each is merged back into `main` with a merge commit. A beta tag `vX.Y.Z-beta.N` publishes a GitHub pre-release from `docs/releases/X.Y.Z-beta.N.md` and needs no dated CHANGELOG section.

Testers who need an unreleased build still build from source or use `/deploy-remote-test`; commits between tags report the last released version.

## Further reading

- [Architecture](architecture.md) — system design, three-layer pattern, component overview
- [Troubleshooting](troubleshooting.md) — common build and runtime issues
- [AGENTS.md](../AGENTS.md) — AI driver development guide; vendor-specific notes are indexed in [agent-instructions.md](agent-instructions.md)
- [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) — ConformU-validated driver matrix
- [ASCOM Alpaca API Specification](https://ascom-standards.org/api/)
- [ConformU](https://github.com/ASCOMInitiative/ConformU) — official conformance testing
