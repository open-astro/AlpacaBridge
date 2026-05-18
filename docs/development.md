# Development Guide

This guide covers building AlpacaBridge from source, running tests, and contributing drivers. For end-user install instructions, see the [README](../README.md).

## Supported platforms

- **Debian 13 (Trixie)** on `arm64` (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate) — the only supported architecture
- Linux arm64 only — no amd64/x86_64, no 32-bit, no Windows or macOS

## Prerequisites

```sh
sudo apt install build-essential cmake g++ \
    libusb-1.0-0-dev libudev-dev \
    nlohmann-json3-dev libcurl4-openssl-dev \
    catch2
```

Verify: `cmake --version` (3.20+), `g++ --version` (GCC 10+, C++20 required).

## Build and run

```sh
chmod +x build_and_run.sh
./build_and_run.sh
```

The server starts on port **6800**: `http://localhost:6800/`

### Manual build

```sh
mkdir build && cd build
cmake .. -DALPACACORE_ENABLE_ALL_VENDORS=ON
cmake --build . --parallel
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
| `ALPACACORE_ENABLE_PLAYERONE` | `OFF` | Player One cameras |
| `ALPACACORE_ENABLE_GEMINI` | `OFF` | Losmandy Gemini focusers |
| `ALPACACORE_ENABLE_WEEWX` | `OFF` | WeeWX observing conditions |

## Running tests

```sh
./run_all_tests.sh
```

Or manually:

```sh
cd build && ctest
```

Filter by tag: `./build/tests/alpacacore_tests [zwo][camera]`
Exclude hardware tests: `./build/tests/alpacacore_tests ~[hardware]`

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

    REQUIRE(driver->get_camera_state() == alpacacore::CameraState::Idle);
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
- `CameraState` is `Idle` before first exposure
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

For the full interactive workflow, use the `/driver-build` skill in Claude Code — it walks through every step from SDK placement through ConformU validation. See [AGENTS.md](../AGENTS.md) for architecture rules and vendor-specific lessons learned.

## Claude Code skills

AlpacaBridge ships with Claude Code skills (slash commands) that automate common development workflows. These are defined in `.claude/commands/` and available when using Claude Code in this repository.

### `/driver-build`

Guided driver implementation assistant. Walks through building, extending, or fixing device drivers:

- Interactive Q&A: device type, vendor, connection method, SDK availability
- Searches INDI/INDIGO for reference drivers with fuzzy vendor name matching
- Creates feature branch (`driver/<vendor>-<device>`)
- Guides through the 3-layer architecture, SDK cleanup, auto-detection, CMake setup
- Enforces Catch2 tests (8 cases, 30+ assertions) and ASCOM Alpaca API compliance
- Validates with ConformU 4.3.0 on arm64
- Updates AGENTS.md with lessons learned

### `/commit`

Stage, review, and commit changes with proper formatting:

- Assesses the working tree, reviews diffs, flags red flags (SDK bloat, secrets, build artifacts)
- Updates SUPPORTED-DRIVERS.md if driver or ConformU changes are present
- Updates CHANGELOG.md with entries under the UNRELEASED version
- Writes verb-first commit messages with vendor/device specificity
- Suggests logical commit splits (driver code vs ConformU vs docs vs SDK)

### `/submit-pr`

Submit a pull request from the current feature branch:

- Safety checks: refuses to PR from `main`, blocks on uncommitted changes
- Auto-detects direct contributor vs fork, handles both flows
- Runs pre-submission checklist (tests, ConformU, CHANGELOG, SUPPORTED-DRIVERS, AGENTS.md, license headers, SDK cleanup)
- Builds PR title and body with component-tagged changes and test plan
- Pushes branch and creates PR via `gh`

## Installing a source build as a service

```sh
./install_alpaca_service.sh install    # build + install systemd service
./install_alpaca_service.sh update     # rebuild + restart
./install_alpaca_service.sh uninstall  # remove service
./install_alpaca_service.sh status     # show status
```

For production use, prefer the apt package from [apt.openastro.net](https://apt.openastro.net).

## Packaging

The Debian package is built from the `debian/` directory. It installs:

- Binary at `/usr/bin/alpacabridge`
- Vendor libraries under `/usr/lib/alpacabridge`
- Configuration under `/etc/alpacabridge`
- Systemd unit `alpacabridge.service` running as the `alpacabridge` system user

## Further reading

- [Architecture](architecture.md) — system design, three-layer pattern, component overview
- [Troubleshooting](troubleshooting.md) — common build and runtime issues
- [AGENTS.md](../AGENTS.md) — AI driver development guide and vendor-specific notes
- [SUPPORTED-DRIVERS.md](../SUPPORTED-DRIVERS.md) — ConformU-validated driver matrix
- [ASCOM Alpaca API Specification](https://ascom-standards.org/api/)
- [ConformU](https://github.com/ASCOMInitiative/ConformU) — official conformance testing
