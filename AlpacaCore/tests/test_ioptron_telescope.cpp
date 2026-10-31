// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <alpacacore/version.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#endif
#include "fake_ioptron_mount.h"
#include "fake_pty_write.h"
#include "fake_task_clock.h"
#include "gps_status_check.h"
#include "sidereal_time_check.h"

using alpacacore::DeviceType;

namespace {

using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("iOptron Telescope Driver - Defaults", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE(driver->get_can_slew_alt_az());
    REQUIRE(driver->get_can_slew_alt_az_async());
    REQUIRE(driver->get_can_sync());
    REQUIRE_FALSE(driver->get_can_sync_alt_az());
    REQUIRE(driver->get_can_find_home());
    REQUIRE(driver->get_can_park());
    REQUIRE(driver->get_can_unpark());
    REQUIRE(driver->get_can_set_park());
    REQUIRE(driver->get_can_pulse_guide());
    REQUIRE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_set_tracking());
}

// Falsified by: AlpacaCore/include/alpacacore/util/pointing.h validate_ra, validate_latitude: changing either message
// text fails this case.
TEST_CASE("iOptron Telescope Driver - out of range arguments report the shared message before the connection",
          "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);
    const auto message_of = [](const std::function<void()>& call) {
        try {
            call();
        } catch (const alpacacore::AlpacaException& ex) {
            return std::string(ex.what());
        }
        return std::string("no exception");
    };
    CHECK(message_of([&] {
              driver->sync_to_coordinates(24.0, 0.0);
          }).find("SyncToCoordinates: RightAscension must be in range") != std::string::npos);
    CHECK(message_of([&] { driver->set_site_latitude(90.1); }).find("SiteLatitude must be in range") !=
          std::string::npos);
    CHECK(message_of([&] { driver->set_site_longitude(180.1); }).find("SiteLongitude must be in range") !=
          std::string::npos);
}

TEST_CASE("iOptron Telescope Driver - Target Range Validation", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    // When disconnected, get and set target throw (connection required)
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    // Parameter validation precedes the connection check (ASCOM precedence).
    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(25.0); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(100.0); }, alpacacore::AlpacaError::InvalidValue);

    // An in-range value on a disconnected driver is the connection's error.
    require_alpaca_error([&]() { driver->set_target_right_ascension(12.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_target_declination(45.0); }, alpacacore::AlpacaError::NotConnected);

    // Other static ranges: tracking rate, MoveAxis axis and rate, coordinates.
    require_alpaca_error([&]() { driver->set_tracking_rate(5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_tracking_rate(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move_axis(2, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move_axis(0, 99.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->sync_to_coordinates(25.0, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_destination_side_of_pier(25.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_alt_az(90.1, 0.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_alt_az_async(0.0, 360.0); }, alpacacore::AlpacaError::InvalidValue);

    // PulseGuide: direction 0-3 and duration 0-99999 ms (the protocol's 5-digit field).
    require_alpaca_error([&]() { driver->pulse_guide(4, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->pulse_guide(-1, 100); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->pulse_guide(0, -1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100000); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron Telescope Driver - Axis Rate Ranges", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.second >= primary.first);

    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.second >= secondary.first);

    // Tertiary axis not supported; driver returns empty ranges (ConformU expects no 0..0 range).
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    // iOptron returns (0,0) for invalid axis rather than throwing
    auto invalid_axis_range = driver->get_axis_rate_range(2);
    REQUIRE(invalid_axis_range.first == 0.0);
    REQUIRE(invalid_axis_range.second == 0.0);

    // Out-of-range axis raises InvalidValue from AxisRates, not an empty list (#516).
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("iOptron Telescope Driver - Disconnected Behavior", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_THROWS(driver->get_right_ascension());
    REQUIRE_THROWS(driver->get_declination());
    REQUIRE_THROWS(driver->get_altitude());
    REQUIRE_THROWS(driver->get_azimuth());
}

TEST_CASE("iOptron Telescope Driver - Device metadata", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(3, conn);

    CHECK(driver->get_description() == "iOptron CEM120,70,40,26, GEM, HEM, HAE, HAZ series and SkyHunter Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron Driver v1.0");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "iOptron_3");
}

TEST_CASE("iOptron Telescope Driver - Telescope Properties", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);

    auto eq = driver->get_equatorial_system();
    CHECK((eq == alpacacore::EquatorialSystem::Topocentric ||
           eq == alpacacore::EquatorialSystem::J2000 ||
           eq == alpacacore::EquatorialSystem::Other));

    auto align = driver->get_alignment_mode();
    CHECK((align == alpacacore::AlignmentMode::AltAz ||
           align == alpacacore::AlignmentMode::Polar ||
           align == alpacacore::AlignmentMode::GermanPolar));
}

TEST_CASE("iOptron Telescope Driver - ASCOM Error Codes", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// GOTO final-approach RA trim over a scripted loopback mount (see
// fake_ioptron_mount.h). The fake lands every GOTO 12 arcsec east of the
// target, the HAE29C/HAE16 firmware signature; goto_refine_active() must
// close it with a :ZQ pulse on the gated models and leave other models alone.
// ---------------------------------------------------------------------------

namespace {

alpacacore::vendor::ioptron::ConnectionInfo loopback_endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    return info;
}

// The async GOTO is dispatched on the driver's worker thread over real I/O. Wait for it to reach the mount
// before stepping the virtual clock: a dispatch that lands after the step re-stamps the 5 s Slewing override
// from the later virtual time, so the case would pass (or fail) on the race and not on the guard it pins.
bool wait_for_goto_dispatch(alpacacore::test::FakeIoptronMount& mount) {
    const auto dispatch_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (mount.count(":MS1#") < 1) {
        if (std::chrono::steady_clock::now() >= dispatch_by) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Slew, then poll Slewing until the driver reports the GOTO complete. The
// driver holds Slewing true for its 5 s post-dispatch override before it
// consults the mount, so allow comfortably more than that.
bool slew_and_settle(alpacacore::TelescopeDriver& driver, alpacacore::test::FakeTaskClock& clock,
                     alpacacore::test::FakeIoptronMount& mount) {
    driver.slew_to_coordinates_async(12.0, 20.0);
    if (!wait_for_goto_dispatch(mount)) {
        return false;
    }
    const auto deadline = clock.now() + std::chrono::seconds(20);
    while (clock.now() < deadline) {
        if (!driver.get_slewing()) {
            return true;
        }
        clock.advance(std::chrono::milliseconds(100));
    }
    return false;
}

}  // namespace

// Connected, so a deleted validate_ra()/validate_dec() cannot hide behind the
// NotConnected the disconnected case above would throw anyway.
TEST_CASE("iOptron Telescope Driver - Target Range Validation over the fake mount",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);

    // A rejected write must not mark the target as set.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
    CHECK(driver->get_target_right_ascension() == Catch::Approx(12.0));
    CHECK(driver->get_target_declination() == Catch::Approx(45.0));
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - all four equatorial slews are refused while Tracking is false",
          "[ioptron][telescope][unit][fake][tracking-guard]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(0);  // stopped: the firmware reports no tracking
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_FALSE(driver->get_tracking());

    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 95.0); },
                         alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(12.0);
    driver->set_target_declination(20.0);
    require_alpaca_error([&]() { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // Nothing reached the mount, and the driver is not slewing.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(mount.count(":MS1#") == 0);
    CHECK(mount.count(":MS2#") == 0);
    CHECK_FALSE(driver->get_slewing());

    // With the mount tracking the same call goes through.
    mount.set_system_status(1);
    driver->set_tracking(true);
    REQUIRE(driver->get_tracking());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 20.0));
    CHECK(wait_for_goto_dispatch(mount));
    driver->set_connected(false);
}

// :GLS reports a GOTO (2), guiding (3) or a meridian flip (4) in place of tracking, and the Tracking getter
// reads them as off: a slew then is still a slew with Tracking on, so the guard must accept it. A commanded
// Tracking=false and a stopped mount at home (7) are refused.
TEST_CASE("iOptron Telescope Driver - a slew while the mount reports motion is not refused for Tracking",
          "[ioptron][telescope][unit][fake][tracking-guard]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(1);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_tracking());

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(12.0, 20.0));
    REQUIRE(wait_for_goto_dispatch(mount));
    mount.set_system_status(2);
    clock.advance(std::chrono::seconds(31));  // past the connect fast-cache grace and the status TTL
    REQUIRE_FALSE(driver->get_tracking());    // the getter reads the slewing status as Tracking off
    CHECK(driver->get_slewing());

    const auto wait_for_gotos = [&](int n) {
        const auto dispatch_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (mount.count(":MS1#") < n && std::chrono::steady_clock::now() < dispatch_by) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return mount.count(":MS1#");
    };
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(13.0, 25.0));  // retarget of the GOTO in flight
    CHECK(wait_for_gotos(2) == 2);

    // Guiding (3) and a meridian flip (4) read as off too, and are not refused either.
    int gotos = 2;
    for (const int status : {3, 4}) {
        INFO("system status " << status);
        mount.set_system_status(status);
        clock.advance(std::chrono::seconds(2));  // past the status TTL
        REQUIRE_FALSE(driver->get_tracking());
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(13.0, 25.0));
        ++gotos;
        CHECK(wait_for_gotos(gotos) == gotos);
    }

    // A commanded Tracking=false is off while the status still reads slewing.
    driver->set_tracking(false);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(14.0, 25.0); },
                         alpacacore::AlpacaError::InvalidOperation);

    // Stopped at home is off: refused, with nothing more sent.
    mount.set_system_status(7);
    clock.advance(std::chrono::seconds(3));  // past the 2 s Tracking read-back window
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(14.0, 25.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(mount.count(":MS1#") == gotos);
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - HAE16 EQ (0012) GOTO settle is closed by the pulse-guide trim",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron HAE16 EQ");

    REQUIRE(slew_and_settle(*driver, clock, mount));

    // One westward (RA-) trim pulse closed the 12 arcsec east residual;
    // the fake then reports the target exactly, so no further trims.
    CHECK(mount.count(":ZQ") == 1);
    CHECK(mount.count(":ZS") == 0);
    CHECK(std::abs(driver->get_right_ascension() - 12.0) * 15.0 * 3600.0 < 1.0);

    // A disconnect right after a fresh GOTO must not leak the 5 s Slewing
    // override into the next connection (PR #228 review).
    driver->slew_to_coordinates_async(13.0, 20.0);
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_slewing());

    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - HEM27 (0025) GOTO settle is left to the firmware (no trim)",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron HEM27");

    REQUIRE(slew_and_settle(*driver, clock, mount));

    // Not a gated model: the residual is reported as-is and no pulse is sent.
    CHECK(mount.count(":ZQ") == 0);
    CHECK(mount.count(":ZS") == 0);
    CHECK(std::abs(driver->get_right_ascension() - 12.0) * 15.0 * 3600.0 > 10.0);

    driver->set_connected(false);
}
// open-astro#346, the shape #304 fixed on the Sky-Watcher driver: ASCOM treats
// TargetRightAscension and TargetDeclination as independent properties, each
// throwing ValueNotSet until that property itself has been written. One shared
// flag let a write to either unlock both, so a client reading the target it
// did not set got a default 0 rather than an error -- which ConformU reports as
// "Read before write should generate an error and didn't".
TEST_CASE("iOptron Telescope Driver - the two target properties are independent", "[ioptron][telescope][unit][fake]") {
    // Over the fake mount rather than disconnected: this driver's target
    // setters write the value to the mount (:SRA / :Sd), so they need a live
    // connection -- the four sibling drivers only store it.
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // Neither written yet: both refuse.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // RA alone unlocks RA alone. This is the assertion that fails on one
    // shared flag: Dec used to read back 0.0 here.
    driver->set_target_right_ascension(7.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // SlewToTarget, SlewToTargetAsync and SyncToTarget still need BOTH halves:
    // a half-set pair must refuse rather than slew to a default Dec.
    require_alpaca_error([&]() { driver->slew_to_target(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->sync_to_target(); }, alpacacore::AlpacaError::ValueNotSet);

    // Once Dec is written too, both read back.
    driver->set_target_declination(-30.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    CHECK(driver->get_target_declination() == -30.25);

    // A reconnect clears both, so the next session starts from "unset" the
    // same way -- the connect/disconnect resets write the pair, not one half.
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    driver->set_connected(false);
}

// open-astro#728: the fault latch counted every failed read over the whole
// session, so three transient failures spread over a night latched it, and the
// latch then refused AbortSlew too. SyncToCoordinates is the probe because it
// forces a fresh :GEP position read on every call; the connect grace would
// serve every other getter from cache.
namespace {

constexpr const char* kReadFailure = "Failed to refresh mount position";
constexpr const char* kLatched = "Mount communications compromised";

// The DriverException message of one forced position read, or "" on success.
std::string sync_failure(alpacacore::TelescopeDriver& driver) {
    try {
        driver.sync_to_coordinates(12.0, 20.0);
        return "";
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == alpacacore::AlpacaError::DriverException);
        return ex.what();
    }
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("iOptron Telescope Driver - failures separated by a successful read never latch (#728)",
          "[ioptron][telescope][unit][fake][fault]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.set_fail_reads(true);
    CHECK(contains(sync_failure(*driver), kReadFailure));
    CHECK(contains(sync_failure(*driver), kReadFailure));
    mount.set_fail_reads(false);
    CHECK(sync_failure(*driver).empty());

    // Two more failures: four in the session, never three in a row.
    mount.set_fail_reads(true);
    CHECK(contains(sync_failure(*driver), kReadFailure));
    CHECK(contains(sync_failure(*driver), kReadFailure));
    mount.set_fail_reads(false);
    CHECK(sync_failure(*driver).empty());
    CHECK_NOTHROW(driver->get_right_ascension());

    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - three consecutive failed reads latch the fault (#728)",
          "[ioptron][telescope][unit][fake][fault]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // An earlier failure followed by a success does not count toward the run.
    mount.set_fail_reads(true);
    CHECK(contains(sync_failure(*driver), kReadFailure));
    mount.set_fail_reads(false);
    CHECK(sync_failure(*driver).empty());

    mount.set_fail_reads(true);
    CHECK(contains(sync_failure(*driver), kReadFailure));
    CHECK(contains(sync_failure(*driver), kReadFailure));
    CHECK(contains(sync_failure(*driver), kReadFailure));  // the third in a row latches

    // Latched: members that run the connection check refuse with the same message, even once the link answers.
    mount.set_fail_reads(false);
    CHECK(contains(sync_failure(*driver), kLatched));
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::DriverException);

    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - AbortSlew sends the stop on a latched fault and clears it (#728)",
          "[ioptron][telescope][unit][fake][fault]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) {
        CHECK(contains(sync_failure(*driver), kReadFailure));
    }
    REQUIRE(contains(sync_failure(*driver), kLatched));
    CHECK_FALSE(driver->get_link_fault().empty());

    // Reads still fail and the cached status says "not slewing": the stop
    // must go out anyway, with no status read in front of it.
    const auto before = mount.commands().size();
    REQUIRE_NOTHROW(driver->abort_slew());
    const auto commands = mount.commands();
    int stops = 0;
    int status_reads = 0;
    for (auto i = before; i < commands.size(); ++i) {
        stops += commands[i] == ":Q#" ? 1 : 0;
        status_reads += commands[i] == ":GLS#" ? 1 : 0;
    }
    CHECK(stops == 1);
    CHECK(status_reads == 0);

    // The stop was sent, so the latch is gone, and the management listing
    // stops reporting it.
    mount.set_fail_reads(false);
    CHECK_NOTHROW(driver->get_right_ascension());
    CHECK(sync_failure(*driver).empty());
    CHECK(driver->get_link_fault().empty());

    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - AbortSlew whose stop fails on a latched fault keeps the latch (#728)",
          "[ioptron][telescope][unit][fake][fault]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) {
        CHECK(contains(sync_failure(*driver), kReadFailure));
    }
    REQUIRE(contains(sync_failure(*driver), kLatched));

    // The link is gone, so the blind :Q# cannot be written: the stop was not
    // sent, AbortSlew must say so, and the latch must stay set.
    mount.reset_link();
    try {
        driver->abort_slew();
        FAIL("AbortSlew succeeded on a reset link");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(contains(ex.what(), "AbortSlew failed"));
    }
    CHECK(contains(sync_failure(*driver), kLatched));
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::DriverException);

    driver->set_connected(false);
}

#endif  // !_WIN32

#ifndef _WIN32

// ── The client-clock disagreement warning (#409) ─────────────────────────────

TEST_CASE("iOptron Telescope Driver - a far-off client UTCDate is logged once per connection on a disciplined host",
          "[ioptron][telescope][unit]") {
    // Same contract as the OnStep case: the mount keeps its own clock and
    // UTCDate writes it, so the driver keeps aiming by the client's instant;
    // what it adds is the shared once-per-connection WARN on an
    // NTP-disciplined host. Without this case, deleting this driver's
    // warn_once() call left the suite green (review finding on #471).
    struct ProbeGuard {
        ProbeGuard() {
            alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([] { return true; });
        }
        ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
    } probe_guard;
    std::atomic<int> warns{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn && component == "iOptron" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    // :SUT is sent blind (some mounts never answer it), so the default
    // canned responder is enough.
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto far = std::chrono::system_clock::now() + std::chrono::minutes(30);
    driver->set_utc_date(std::chrono::system_clock::now());  // agrees: no line, budget untouched
    CHECK(warns.load() == 0);
    driver->set_utc_date(far);
    CHECK(warns.load() == 1);
    driver->set_utc_date(far + std::chrono::seconds(1));
    CHECK(warns.load() == 1);

    // A reconnect re-arms it.
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_utc_date(far);
    CHECK(warns.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - UTCDate stays unset until time is established",
          "[ioptron][telescope][unit][utcdate]") {
    alpacacore::test::FakeIoptronMount server("0025", 0.0);
    const auto invalid = GENERATE("+0000xxxxxxxxxxxxx#", "+00009999999999999#", "+00020000000000000#");
    server.set_utc_reply(invalid);  // Site metadata remains readable; UTC payload is invalid.
    REQUIRE(server.ok());
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, conn, std::nullopt, std::nullopt,
                                                                                  std::nullopt, false, clock);
    for (int session = 0; session < 2; ++session) {
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        REQUIRE_NOTHROW(driver->get_sidereal_time());
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
        CHECK(server.count(":GUT") == 2 * (session + 1));  // Clock sample and the existing site-metadata read.
        driver->set_utc_date(std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)});
        CHECK(driver->get_utc_date() == std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)});
        driver->set_connected(false);
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
    }
}

TEST_CASE("iOptron Telescope Driver - connect samples mount UTC before background sync",
          "[ioptron][telescope][unit][utcdate]") {
    const bool sync_time = GENERATE(false, true);
    alpacacore::test::FakeIoptronMount mount("0025", 0.0);
    mount.set_utc_reply("-18010000000000000#");  // UTC J2000, even with nonzero timezone and DST metadata.
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, loopback_endpoint(mount.port()),
                                                                                  {}, {}, {}, sync_time, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto mount_utc = std::chrono::system_clock::time_point{std::chrono::seconds(946728000)};
    CHECK(driver->get_utc_date() == mount_utc);
    CHECK(mount.count(":GUT") == 2);  // Clock sample plus the existing site-metadata read.
    CHECK(mount.count(":SUT") == 0);
    CHECK(driver->get_utc_date() == mount_utc);
    CHECK(mount.count(":GUT") == 2);
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - connect time sample does not block UTC getters or drop disconnect",
          "[ioptron][telescope][unit][utcdate][getter-stall]") {
    std::atomic<bool> entered{false};
    std::promise<void> release;
    auto released = release.get_future().share();
    alpacacore::test::FakeMountServer server([&](const std::string& command) {
        if (command == ":GUT#" && !entered.exchange(true)) released.wait();
        if (command == ":GUT#") return std::string("+00000000000000000#");
        return alpacacore::test::FakeMountServer::default_responder()(command);
    });
    REQUIRE(server.ok());
    auto conn = loopback_endpoint(server.port());
    conn.response_timeout_ms = 2000;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, conn, {}, {}, {}, false, clock);
    auto connecting = std::async(std::launch::async, [&] { driver->set_connected(true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool held = entered.load();
    const bool published = driver->get_connected();
    auto reader = std::async(std::launch::async, [&] {
        try {
            (void)driver->get_utc_date();
            return 0;
        } catch (const alpacacore::AlpacaException& ex) {
            return ex.error_code();
        }
    });
    const auto ready = reader.wait_for(std::chrono::milliseconds(100));
    auto disconnecting = std::async(std::launch::async, [&] { driver->set_connected(false); });
    release.set_value();  // Release every worker before any assertion can abort the case.
    connecting.get();
    disconnecting.get();
    CHECK(held);
    CHECK_FALSE(published);
    CHECK(ready == std::future_status::ready);
    CHECK(reader.get() == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("iOptron Telescope Driver - configured UTC sync publishes only a successful time write",
          "[ioptron][telescope][unit][utcdate]") {
    const bool fail_write = GENERATE(false, true);
    alpacacore::test::FakeIoptronMount mount("0025", 0.0);
    mount.set_utc_reply("bad#");
    REQUIRE(mount.ok());
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = mount.port();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, conn, {}, {}, {}, true, clock);
    if (fail_write) {
        alpacacore::vendor::ioptron::set_clock_sync_hook_for_testing(*driver, [] {
            // A closed transport makes the blind write throw; silence alone is permitted by this protocol.
            alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance().disconnect();
        });
    }
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto before = std::chrono::system_clock::now();
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    if (fail_write) {
        // Three commands, each with two cancellable 750 ms retry waits.
        for (int retry = 0; retry < 6; ++retry) {
            REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
            clock.advance(std::chrono::milliseconds(750));
            REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
        }
        CHECK(mount.count(":SUT") == 0);
        require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    } else {
        CHECK(mount.count(":SUT") == 1);
        CHECK(driver->get_utc_date() >= before);
        CHECK(driver->get_utc_date() <= std::chrono::system_clock::now());
        const auto commands = mount.commands();
        const auto write = std::find_if(commands.begin(), commands.end(),
                                        [](const auto& command) { return command.rfind(":SUT", 0) == 0; });
        REQUIRE(write != commands.end());
        const auto sent = std::chrono::system_clock::time_point{std::chrono::seconds(946728000)} +
                          std::chrono::milliseconds(std::stoll(write->substr(4)));
        CHECK(driver->get_utc_date() >= sent);
        CHECK(driver->get_utc_date() < sent + std::chrono::milliseconds(1));
    }
    driver->set_connected(false);
}

// #1590: the UTCDate readback ages by the driver's TaskClock, not the host's steady clock.
TEST_CASE("iOptron Telescope Driver - the UTCDate readback after a write moves by the advanced TaskClock time",
          "[ioptron][telescope][unit][taskclock]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    alpacacore::test::FakeTaskClock clock;
    // Clock sync off: its body waits on the same clock, so advance() would wake it and it would
    // overwrite the written UTC with the host's.
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, conn, std::nullopt, std::nullopt,
                                                                                  std::nullopt, false, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));

    const auto written = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 365 * 30);
    driver->set_utc_date(written);
    CHECK(driver->get_utc_date() == written);
    clock.advance(std::chrono::seconds(90));
    CHECK(driver->get_utc_date() == written + std::chrono::seconds(90));
    clock.advance(std::chrono::milliseconds(250));
    CHECK(driver->get_utc_date() == written + std::chrono::seconds(90) + std::chrono::milliseconds(250));
    driver->set_connected(false);
}

// #627: `x < min || x > max` is false for NaN. The elevation setter is the one
// iOptron setter that validates without a connection, so it is the one that
// stored NaN on a disconnected driver; the others are covered while connected.
TEST_CASE("iOptron Telescope Driver - non-finite site elevation is rejected", "[ioptron][telescope][unit][nonfinite]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    driver->set_site_elevation(120.0);
    require_alpaca_error([&]() { driver->set_site_elevation(std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_site_elevation() == 120.0);
}

// #627: the guide-rate range check is `fraction < 0 || fraction > 1`, which NaN
// passes, so a NaN rate was stored (and, for iOptron, clamped to NaN and
// written to the mount). The finite check runs before the connection check,
// like every other parameter validation, so a disconnected driver proves it.
TEST_CASE("iOptron Telescope Driver - non-finite guide rate is rejected", "[ioptron][telescope][unit][nonfinite]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    require_alpaca_error([&]() { driver->set_guide_rate({nan, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_guide_rate({0.004, nan}); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_guide_rate({inf, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
}

// #627: the NaN path of iOptron's site latitude and longitude setters, over a
// connected driver. The disconnected range case is in Target Range Validation.
TEST_CASE("iOptron Telescope Driver - non-finite site latitude and longitude are rejected",
          "[ioptron][telescope][unit][nonfinite]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    conn.response_timeout_ms = 50;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    const double nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("SiteLatitude") {
        require_alpaca_error([&]() { driver->set_site_latitude(nan); }, alpacacore::AlpacaError::InvalidValue);
    }
    SECTION("SiteLongitude") {
        require_alpaca_error([&]() { driver->set_site_longitude(nan); }, alpacacore::AlpacaError::InvalidValue);
    }
    driver->set_connected(false);
}

#endif  // _WIN32

// Issue #995: the mount's serial link is held exclusively while connected and
// released on disconnect. connect() over serial only opens and configures the
// port, so a bare pty stands in for the adapter.
TEST_CASE("iOptron mount serial - the connected port is claimed exclusively and released on disconnect",
          "[ioptron][telescope][unit]") {
    if (geteuid() == 0) {
        SUCCEED("root bypasses TIOCEXCL");
        return;
    }
    alpacacore::test::PtyPair pty("iOptron mount exclusive test");
    auto& protocol = alpacacore::vendor::ioptron::iOptronProtocolWrapper::instance();
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    info.port_path = pty.slave_path();
    REQUIRE(protocol.connect(info));

    errno = 0;
    int fd = open(pty.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const int err = errno;
    if (fd >= 0) close(fd);
    CHECK(fd < 0);
    CHECK(err == EBUSY);

    protocol.disconnect();
    fd = open(pty.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

// Issue #843: a HAE29C reports system status 2 (slewing) while a long PulseGuide runs, and the guider
// stopped on Slewing == true. Slewing stays false for the pulse window (duration plus the driver's
// settle delay); IsPulseGuiding reports the pulse.
TEST_CASE("iOptron Telescope Driver - Slewing stays false while a PulseGuide runs",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    // The firmware already reports "slewing" when the driver first reads the status (the read is cached).
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->pulse_guide(3, 2704);
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - firmware motion with no PulseGuide still reports Slewing",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    // The firmware already reports "slewing" when the driver first reads the status (the read is cached).
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // A slew started from the handset: firmware moving, no driver-initiated motion, no pulse.
    CHECK(driver->get_slewing());

    // A short pulse that has ended (duration plus settle delay) no longer masks the firmware status.
    driver->pulse_guide(3, 50);
    clock.advance(std::chrono::milliseconds(1300));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_slewing());

    // A driver-initiated GOTO started during a pulse keeps Slewing true.
    driver->pulse_guide(3, 9000);
    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    CHECK(driver->get_slewing());

    // Past the 5 s Slewing override only the firmware status (still 2) holds Slewing true; the open pulse
    // window must not mask it while the GOTO is in flight.
    clock.advance(std::chrono::milliseconds(7000));
    CHECK(driver->get_is_pulse_guiding());
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver - Slewing stays false in the settle window after a PulseGuide",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // The pulse duration has passed but the driver's 1 s completion delay has not: the firmware is still
    // reporting the pulse, as a guider reading Slewing right after its pulse sees it.
    driver->pulse_guide(3, 50);
    clock.advance(std::chrono::milliseconds(300));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Review of #843: Park and FindHome set no slew_in_progress_, so once their 5 s Slewing override ended only
// the firmware status held Slewing true, and a PulseGuide accepted mid-motion masked it.
TEST_CASE("iOptron Telescope Driver - Slewing stays true when a PulseGuide arrives during a park",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    for (const char* model : {"0036", "0040"}) {
        CAPTURE(model);
        alpacacore::test::FakeIoptronMount mount(model, /*landing_ra_error_arcsec=*/0.0);
        REQUIRE(mount.ok());
        mount.set_system_status(2);
        alpacacore::test::FakeTaskClock clock;
        auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());

        driver->park();
        clock.advance(std::chrono::milliseconds(5500));
        CHECK(driver->get_slewing());
        driver->pulse_guide(3, 3000);
        CHECK(driver->get_is_pulse_guiding());
        CHECK(driver->get_slewing());
        driver->set_connected(false);
    }
}

TEST_CASE("iOptron Telescope Driver - Slewing stays true when a PulseGuide arrives during a FindHome",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->find_home();
    clock.advance(std::chrono::milliseconds(5500));
    CHECK(driver->get_slewing());
    driver->pulse_guide(3, 3000);
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

// Review of #843: MoveAxis(axis, 0) sends :qR#/:qD#, which stop arrow motion only; the iOptron RS-232 command
// language says other slewing is not affected, so the park keeps moving and must not be masked by a PulseGuide.
// A jog before the stop does not end it either.
TEST_CASE("iOptron Telescope Driver - Slewing stays true for a PulseGuide after MoveAxis during a park",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->park();
    driver->move_axis(0, driver->get_axis_rate_range(0).first);
    driver->move_axis(0, 0.0);
    driver->move_axis(1, 0.0);
    driver->pulse_guide(3, 3000);
    CHECK(driver->get_is_pulse_guiding());
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

// Same for a GOTO: MoveAxis supersedes the dispatch (slew_in_progress_ cleared), but the mount keeps slewing.
TEST_CASE("iOptron Telescope Driver - Slewing stays true for a PulseGuide after MoveAxis rate 0 during a GOTO",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    driver->move_axis(0, 0.0);
    driver->pulse_guide(3, 3000);
    CHECK(driver->get_is_pulse_guiding());
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

// Review of #843: a client that waits for FindHome through AtHome never reads Slewing until guiding starts.
// The end of the home motion must still be seen, or the next PulseGuide is not masked.
TEST_CASE("iOptron Telescope Driver - Slewing stays false for a PulseGuide after a FindHome seen through AtHome",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto connected_at = clock.now();

    driver->find_home();
    // Past the 5 s Slewing override and the driver's 30 s post-connect status cache, so the reads below
    // reach the mount.
    clock.advance(connected_at + std::chrono::seconds(31) - clock.now());
    mount.set_system_status(7);
    CHECK(driver->get_at_home());

    driver->set_tracking(true);
    mount.set_system_status(2);
    driver->pulse_guide(3, 3000);
    clock.advance(std::chrono::milliseconds(1100));
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// The firmware can still report its old status for a moment after the home command, so a status read
// inside the 5 s Slewing override must not end the FindHome motion window.
TEST_CASE("iOptron Telescope Driver - Slewing stays true for a PulseGuide during a FindHome read early through AtHome",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(1);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto connected_at = clock.now();
    // Past the driver's 30 s post-connect status cache, so the reads below reach the mount.
    clock.advance(connected_at + std::chrono::seconds(31) - clock.now());

    driver->find_home();
    CHECK_FALSE(driver->get_at_home());  // still the old tracking status
    mount.set_system_status(2);
    clock.advance(std::chrono::milliseconds(5500));
    driver->pulse_guide(3, 3000);
    CHECK(driver->get_is_pulse_guiding());
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

// A FindHome that lands inside its 5 s Slewing override: the at-home status read then ends the motion window,
// so a PulseGuide sent right after is masked once the override has passed.
TEST_CASE("iOptron Telescope Driver - Slewing stays false for a PulseGuide after a quick FindHome",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(1);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto connected_at = clock.now();
    // Past the driver's 30 s post-connect status cache, so the reads below reach the mount.
    clock.advance(connected_at + std::chrono::seconds(31) - clock.now());

    driver->find_home();
    const auto home_at = clock.now();
    mount.set_system_status(7);
    CHECK(driver->get_at_home());

    driver->set_tracking(true);
    mount.set_system_status(2);
    clock.advance(home_at + std::chrono::seconds(2) - clock.now());
    driver->pulse_guide(3, 3000);
    clock.advance(home_at + std::chrono::milliseconds(5500) - clock.now());
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// open-astro#1223: the clock-sync body's 250 ms start delay is a cancellable wait on the driver's clock, so
// a disconnect that lands inside it joins at once. On a FakeTaskClock nobody advances, an uncancellable sleep
// would park the join forever.
TEST_CASE("iOptron Telescope Driver - a disconnect inside the clock-sync start delay does not wait it out",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // The clock-sync body is parked in its 250 ms start delay on the fake clock.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));

    std::promise<void> disconnected;
    auto done = disconnected.get_future();
    std::thread disconnecter([&] {
        driver->set_connected(false);
        disconnected.set_value();
    });
    const bool joined_without_advance = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!joined_without_advance) {
        clock.advance(std::chrono::seconds(1));  // release the parked body so the test can end
    }
    disconnecter.join();
    CHECK(joined_without_advance);
    CHECK_FALSE(driver->get_connected());
}

// open-astro#1223: the 750 ms retry delay after a failed site command is a cancellable wait on the driver's
// clock too. A disconnect that lands inside it joins at once; an uncancellable sleep would park the join
// forever on a FakeTaskClock nobody advances.
TEST_CASE("iOptron Telescope Driver - a disconnect inside the site-sync retry delay does not wait it out",
          "[ioptron][telescope][unit][fake][taskclock]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
        0, loopback_endpoint(mount.port()), -33.856784, 151.215297, std::nullopt, false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // The clock-sync body is parked in its 250 ms start delay.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    // Drop the link, so the first set_latitude send fails and the body lands in its retry delay.
    mount.reset_link();
    const auto waits = clock.wait_count();
    clock.advance(std::chrono::milliseconds(250));
    // The first set_latitude fails; the body registers its retry delay as the next wait.
    REQUIRE(clock.wait_for_wait_count(waits + 1, std::chrono::seconds(5)));

    std::promise<void> disconnected;
    auto done = disconnected.get_future();
    std::thread disconnecter([&] {
        driver->set_connected(false);
        disconnected.set_value();
    });
    const bool joined_without_advance = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    // Release the parked body (one retry delay per attempt) so the test can end.
    while (!joined_without_advance && done.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        clock.advance(std::chrono::seconds(10));
    }
    disconnecter.join();
    CHECK(joined_without_advance);
    CHECK_FALSE(driver->get_connected());
}

namespace {

// Real time: the fake mount answers on the host clock, so the dispatch that runs on the slew slot's thread
// lands in real milliseconds, not on the driver's FakeTaskClock.
bool wait_for_slewing_error(alpacacore::TelescopeDriver& driver, std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            (void)driver.get_slewing();
        } catch (const alpacacore::AlpacaException& ex) {
            message = ex.what();
            return ex.error_code() == alpacacore::AlpacaError::DriverException;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

TEST_CASE("iOptron slot - a GOTO the mount rejects after SlewToCoordinatesAsync returned surfaces on Slewing",
          "[ioptron][telescope][async][slot]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::string message;
    REQUIRE(wait_for_slewing_error(*driver, message));
    CHECK(message == "SlewToCoordinatesAsync failed: mount rejected the target");
    driver->set_connected(false);
}

TEST_CASE("iOptron slot - a GOTO the mount rejects after SlewToAltAzAsync returned surfaces on Slewing",
          "[ioptron][telescope][async][slot]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
        0, loopback_endpoint(mount.port()), -33.856784, 151.215297, std::nullopt, false, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_alt_az_async(30.0, 90.0));
    std::string message;
    REQUIRE(wait_for_slewing_error(*driver, message));
    CHECK(message == "SlewToAltAzAsync failed: mount rejected the target");
    driver->set_connected(false);
}

TEST_CASE("iOptron slot - a refused slew thread throws and Slewing still reports the earlier failure",
          "[ioptron][telescope][async][slot]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
        0, loopback_endpoint(mount.port()), -33.856784, 151.215297, std::nullopt, false, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::string earlier;
    REQUIRE(wait_for_slewing_error(*driver, earlier));
    const int gotos_before = mount.count(":MS1#");

    alpacacore::vendor::ioptron::set_slew_spawn_for_testing(*driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    require_alpaca_error([&] { driver->slew_to_coordinates_async(6.5, 25.0); },
                         alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { driver->slew_to_alt_az_async(30.0, 90.0); }, alpacacore::AlpacaError::DriverException);

    // The refused starts rolled back: the earlier failure is still the one Slewing reports, and no GOTO was sent.
    std::string after;
    REQUIRE(wait_for_slewing_error(*driver, after));
    CHECK(after == earlier);
    CHECK(mount.count(":MS1#") == gotos_before);
    driver->set_connected(false);
}

// #1298: the LST helper cut UTC to whole seconds.
TEST_CASE("iOptron Telescope Driver - SiderealTime follows the clock to under a second",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    // Do not step from the sampled mount epoch to host time during the sub-second measurement.
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, loopback_endpoint(mount.port()),
                                                                                  {}, {}, {}, false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    alpacacore::test::check_sidereal_time_follows_the_clock(*driver);
    driver->set_connected(false);
}

// ── #1478 spec-audit items ───────────────────────────────────────────────────

TEST_CASE("iOptron Telescope Driver spec audit - DoesRefraction is not implemented",
          "[ioptron][telescope][unit][spec-audit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);
    // The driver applies no refraction correction: ITelescopeV4 has it raise on read and on write. While
    // disconnected the connection check comes first (#1919).
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::NotConnected);
}

// Connected, DoesRefraction raises PropertyNotImplemented on read and write (disconnected: NotConnected, above).
TEST_CASE("iOptron Telescope Driver spec audit - connected DoesRefraction is not implemented",
          "[ioptron][telescope][unit][spec-audit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

TEST_CASE("iOptron Telescope Driver spec audit - SiteElevation never set throws InvalidOperation",
          "[ioptron][telescope][unit][spec-audit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);
    // ASCOM time-and-place FAQ (decision 0019): a site value never set reads as InvalidOperation, not 0 m.
    require_alpaca_error([&] { (void)driver->get_site_elevation(); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_site_elevation(42.0);
    CHECK(driver->get_site_elevation() == 42.0);
}

// Issue #1277: Action("gps_status") reports whether the mount's GPS receiver holds a fix (:GLS# digit 2), with the
// receiver's own latitude and longitude from the same reply. Without a fix the stored site in that reply is not
// GPS data and is never reported.
TEST_CASE("iOptron Telescope Driver - gps_status reports the GPS fix from :GLS#", "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));

    // Not connected: not listed, not supported, and the call is NotConnected.
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("gps_status"));
    require_alpaca_error([&]() { (void)driver->action("gps_status", ""); }, alpacacore::AlpacaError::NotConnected);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_supported_actions() == std::vector<std::string>{"gps_status"});
    CHECK(driver->can_action("gps_status"));
    CHECK(driver->can_action("GPS_Status"));  // action names are case-insensitive
    CHECK(driver->action("GPS_STATUS", "") == driver->action("gps_status", ""));
    require_alpaca_error([&]() { (void)driver->action("other_action", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);

    SECTION("valid GPS data is a fix with the receiver's site") {
        mount.set_gps(2, -33.856784, 151.215297);
        const std::string reply = driver->action("gps_status", "");
        CHECK(reply.rfind("{\"fix\":true,", 0) == 0);
        REQUIRE(alpacacore::test::gps_status_number(reply, "latitude").has_value());
        CHECK(*alpacacore::test::gps_status_number(reply, "latitude") == Catch::Approx(-33.856784).margin(1e-6));
        CHECK(*alpacacore::test::gps_status_number(reply, "longitude") == Catch::Approx(151.215297).margin(1e-6));
        CHECK_FALSE(alpacacore::test::gps_status_number(reply, "elevation").has_value());
        CHECK(reply.find("\"elevation\":null") != std::string::npos);
    }
    SECTION("a working receiver without valid data is no fix, and the stored site is withheld") {
        mount.set_gps(1, 51.477833, -0.001500);
        CHECK(driver->action("gps_status", "") ==
              "{\"fix\":false,\"latitude\":null,\"longitude\":null,\"elevation\":null}");
    }
    SECTION("no or failed GPS module is no fix") {
        mount.set_gps(0, 51.477833, -0.001500);
        CHECK(driver->action("gps_status", "") ==
              "{\"fix\":false,\"latitude\":null,\"longitude\":null,\"elevation\":null}");
    }
    driver->set_connected(false);
}

// Issue #1277, decision 0016: the gps_status action reads the wire without holding mutex_, so a mutex_ getter
// answers at once while the :GLS# reply is held, and the action sends :GLS# only (no :GUT#).
TEST_CASE("iOptron Telescope Driver - gps_status does not hold mutex_ across the :GLS# exchange",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    mount.set_gps(2, -33.856784, 151.215297);
    const int gut_before = mount.count(":GUT");

    mount.hold_next_reply(std::chrono::milliseconds(1000));
    auto reply = std::async(std::launch::async, [&] { return driver->action("gps_status", ""); });
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver->get_connected());  // lock-free (#910)
    CHECK(driver->can_action("gps_status"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));

    CHECK(reply.get().rfind("{\"fix\":true,", 0) == 0);
    CHECK(mount.count(":GUT") == gut_before);
    driver->set_connected(false);
}

// Issue #1277, decision 0016: SupportedActions and CanAction read the atomic connected_ without mutex_, so
// a CommandString exchange (which holds mutex_ across the wire) does not stall them.
TEST_CASE("iOptron Telescope Driver - SupportedActions and CanAction do not wait on a held command exchange",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.hold_next_reply(std::chrono::milliseconds(1000));
    auto reply = std::async(std::launch::async, [&] { return driver->command_string(":GLS#", true); });
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver->get_supported_actions() == std::vector<std::string>{"gps_status"});
    CHECK(driver->can_action("gps_status"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));

    (void)reply.get();
    driver->set_connected(false);
}
