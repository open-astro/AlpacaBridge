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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_mount_server.h"

using alpacacore::DeviceType;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

alpacacore::vendor::bisque::ConnectionInfo loopback(int port) {
    alpacacore::vendor::bisque::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 300;
    return info;
}

std::string answer_one(const std::string&) { return "1#"; }

} // namespace

TEST_CASE("Bisque Telescope Driver - Defaults", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(driver->get_name() == "Bisque Paramount Telescope");

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE_FALSE(driver->get_can_slew_alt_az());
    REQUIRE_FALSE(driver->get_can_slew_alt_az_async());
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

TEST_CASE("Bisque Telescope Driver - Device metadata", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(3, conn);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Bisque Paramount / TheSkyX Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Bisque TheSkyX Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Bisque_3");
}

TEST_CASE("Bisque Telescope Driver - Disconnected Behavior", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    REQUIRE_FALSE(driver->get_connected());

    // Position queries require connection
    CHECK_THROWS_AS(driver->get_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_declination(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_altitude(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_azimuth(), alpacacore::AlpacaException);

    // Tracking requires connection
    CHECK_THROWS_AS(driver->get_tracking(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->set_tracking(true), alpacacore::AlpacaException);

    // Slew requires target set (throws before connection check)
    CHECK_THROWS_AS(driver->slew_to_target_async(), alpacacore::AlpacaException);

    // Park requires connection
    CHECK_THROWS_AS(driver->park(), alpacacore::AlpacaException);

    // Action and command methods
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Bisque Telescope Driver - Unsupported actions", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Bisque Telescope Driver - Target Range Validation", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    // Target not yet set
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    // RA range: [0, 24)
    REQUIRE_THROWS(driver->set_target_right_ascension(-0.1));
    REQUIRE_THROWS(driver->set_target_right_ascension(24.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));

    // Dec range: [-90, 90]
    REQUIRE_THROWS(driver->set_target_declination(-90.1));
    REQUIRE_THROWS(driver->set_target_declination(90.1));
    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(0.0));
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
}

TEST_CASE("Bisque Telescope Driver - Axis Rate Ranges", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    // Primary axis (RA/Azimuth)
    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.first == 0.0);
    REQUIRE(primary.second > primary.first);

    // Secondary axis (Dec/Altitude)
    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.first == 0.0);
    REQUIRE(secondary.second > secondary.first);

    // Tertiary axis not supported
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());

    REQUIRE_THROWS(driver->get_axis_rate_range(2));

    // Out-of-range axis raises InvalidValue from AxisRates (#516).
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { (void)driver->get_axis_rate_ranges(3); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Bisque Telescope Driver - Target Coordinate Persistence", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);

    REQUIRE_NOTHROW(driver->set_target_declination(45.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(6.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 6.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(0.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 0.0);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(23.999));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 23.999);

    REQUIRE_NOTHROW(driver->set_target_declination(-90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -90.0);
    REQUIRE_NOTHROW(driver->set_target_declination(90.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 90.0);
}

TEST_CASE("Bisque Telescope Driver - Site Property Validation", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_latitude(35.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_latitude(), 35.0);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_longitude(-106.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_longitude(), -106.0);
}

TEST_CASE("Bisque Telescope Driver - Telescope Properties", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);

    auto eq = driver->get_equatorial_system();
    CHECK((eq == alpacacore::EquatorialSystem::Topocentric ||
           eq == alpacacore::EquatorialSystem::J2000 ||
           eq == alpacacore::EquatorialSystem::Other));

    auto align = driver->get_alignment_mode();
    CHECK((align == alpacacore::AlignmentMode::AltAz ||
           align == alpacacore::AlignmentMode::Polar ||
           align == alpacacore::AlignmentMode::GermanPolar));

    auto rates = driver->get_tracking_rates();
    CHECK_FALSE(rates.empty());

    CHECK(driver->get_slew_settle_time() >= 0);
    require_alpaca_error([&]() { driver->set_slew_settle_time(-1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Bisque Telescope Driver - ASCOM Error Codes", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;

    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
}

// open-astro#346, the shape #304 fixed on the Sky-Watcher driver: ASCOM treats
// TargetRightAscension and TargetDeclination as independent properties, each
// throwing ValueNotSet until that property itself has been written. One shared
// flag let a write to either unlock both, so a client reading the target it
// did not set got a default 0 rather than an error -- which ConformU reports as
// "Read before write should generate an error and didn't".
TEST_CASE("Bisque Telescope Driver - the two target properties are independent", "[bisque][telescope][unit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    // Neither written yet: both refuse.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // RA alone unlocks RA alone. This is the assertion that fails on one
    // shared flag: Dec used to read back 0.0 here.
    driver->set_target_right_ascension(7.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // SlewToTarget, SlewToTargetAsync and SyncToTarget still need BOTH halves:
    // a half-set pair must refuse rather than slew to a default Dec. These
    // guards run before the connection check, so they hold on a disconnected
    // driver.
    require_alpaca_error([&]() { driver->slew_to_target(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { driver->sync_to_target(); }, alpacacore::AlpacaError::ValueNotSet);

    // Once Dec is written too, both read back.
    driver->set_target_declination(-30.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    CHECK(driver->get_target_declination() == -30.25);
}

// #627: `x < min || x > max` is false for NaN, so NaN passed every range check
// below and was stored. Every setter validates before it touches the mount, so
// a disconnected driver is enough to prove each one.
TEST_CASE("Bisque Telescope Driver - non-finite input is rejected", "[bisque][telescope][unit][nonfinite]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SECTION("TargetDeclination") {
        require_alpaca_error([&]() { driver->set_target_declination(nan); }, alpacacore::AlpacaError::InvalidValue);
        // Nothing was stored: the property still reads as never written.
        require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    }
    SECTION("TargetRightAscension") {
        require_alpaca_error([&]() { driver->set_target_right_ascension(nan); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); },
                             alpacacore::AlpacaError::ValueNotSet);
    }
    SECTION("SiteElevation") {
        driver->set_site_elevation(120.0);
        require_alpaca_error([&]() { driver->set_site_elevation(nan); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_site_elevation() == 120.0);
    }
    SECTION("SiteLatitude") {
        driver->set_site_latitude(35.0);
        require_alpaca_error([&]() { driver->set_site_latitude(nan); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_site_latitude() == 35.0);
    }
    SECTION("SiteLongitude") {
        driver->set_site_longitude(-106.0);
        require_alpaca_error([&]() { driver->set_site_longitude(nan); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_site_longitude() == -106.0);
    }
    // Bisque's guide-rate setter has no range check at all, so infinity gets
    // through as well as NaN.
    SECTION("GuideRateRightAscension") {
        const auto before = driver->get_guide_rate();
        require_alpaca_error([&]() { driver->set_guide_rate({nan, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_guide_rate({inf, 0.004}); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_guide_rate().ra == before.ra);
    }
    SECTION("GuideRateDeclination") {
        const auto before = driver->get_guide_rate();
        require_alpaca_error([&]() { driver->set_guide_rate({0.004, nan}); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_guide_rate({0.004, -inf}); }, alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_guide_rate().dec == before.dec);
    }
}

// #627: the checks behind the coordinate slew/sync forms and MoveAxis, called
// directly. The driver-level cases below cover the call sites.
TEST_CASE("Bisque Telescope Driver - non-finite slew coordinates and MoveAxis rate are rejected",
          "[bisque][telescope][unit][nonfinite]") {
    using alpacacore::vendor::bisque::detail::validate_move_axis_rate;
    using alpacacore::vendor::bisque::detail::validate_ra_dec;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SECTION("RightAscension") {
        for (const double bad : {nan, inf, -inf}) {
            require_alpaca_error([&]() { validate_ra_dec(bad, 10.0); }, alpacacore::AlpacaError::InvalidValue);
        }
    }
    SECTION("Declination") {
        for (const double bad : {nan, inf, -inf}) {
            require_alpaca_error([&]() { validate_ra_dec(12.0, bad); }, alpacacore::AlpacaError::InvalidValue);
        }
    }
    SECTION("RA is checked before Dec") {
        try {
            validate_ra_dec(nan, nan);
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("RA out of range") != std::string::npos);
        }
    }
    SECTION("MoveAxis rate") {
        for (const double bad : {nan, inf, -inf}) {
            require_alpaca_error([&]() { validate_move_axis_rate(bad); }, alpacacore::AlpacaError::InvalidValue);
        }
    }
    SECTION("finite values in range still pass") {
        CHECK_NOTHROW(validate_ra_dec(0.0, -90.0));
        CHECK_NOTHROW(validate_ra_dec(23.999, 90.0));
        CHECK_NOTHROW(validate_move_axis_rate(0.0));
        CHECK_NOTHROW(validate_move_axis_rate(-2.5));
        CHECK_NOTHROW(validate_move_axis_rate(4.0));
    }
}

// #627: argument validation precedes the connection check (AGENTS.md), so a
// non-finite coordinate or rate is InvalidValue on a disconnected driver too.
TEST_CASE("Bisque Telescope Driver - non-finite slew, sync and MoveAxis arguments are InvalidValue while disconnected",
          "[bisque][telescope][unit][nonfinite]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);
    REQUIRE_FALSE(driver->get_connected());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    namespace AlpacaError = alpacacore::AlpacaError;

    for (const double bad : {nan, inf, -inf}) {
        require_alpaca_error([&]() { driver->slew_to_coordinates(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->sync_to_coordinates(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->sync_to_coordinates(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->move_axis(0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->move_axis(1, bad); }, AlpacaError::InvalidValue);
    }
    // An out-of-range axis is InvalidValue before the connection check as well.
    require_alpaca_error([&]() { driver->move_axis(2, 1.0); }, AlpacaError::InvalidValue);
    // Valid arguments still reach the connection check.
    require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, 10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, 10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync_to_coordinates(12.0, 10.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_axis(0, 1.0); }, AlpacaError::NotConnected);
}

// #627: on a driver connected over FakeMountServer, the same arguments are
// refused before any command reaches TheSkyX.
TEST_CASE("Bisque Telescope Driver - connected slew, sync and MoveAxis refuse non-finite arguments",
          "[bisque][telescope][unit][nonfinite]") {
    std::mutex seen_mutex;
    std::vector<std::string> seen;
    alpacacore::test::FakeMountServer server([&](const std::string& chunk) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(chunk);
        // A wrapped command (try { ... } Out = 'OK#') wants TheSkyX's success
        // text; the handshake and queries take the plain reply.
        return std::string(chunk.find("try {") != std::string::npos ? "|No error. Error = 0.OK#" : "1#");
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    REQUIRE(driver->get_connected());

    auto motion_commands_sent = [&]() {
        std::lock_guard<std::mutex> lock(seen_mutex);
        return std::count_if(seen.begin(), seen.end(), [](const std::string& c) {
            return c.find("SlewToRaDec") != std::string::npos || c.find("Sync(") != std::string::npos ||
                   c.find("DoCommand(9") != std::string::npos;
        });
    };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    namespace AlpacaError = alpacacore::AlpacaError;

    for (const double bad : {nan, inf, -inf}) {
        require_alpaca_error([&]() { driver->slew_to_coordinates(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->slew_to_coordinates_async(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->sync_to_coordinates(bad, 10.0); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->sync_to_coordinates(12.0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->move_axis(0, bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->move_axis(1, bad); }, AlpacaError::InvalidValue);
    }
    CHECK(motion_commands_sent() == 0);
    // Nothing was stored as a target either.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, AlpacaError::ValueNotSet);

    // The fake is live: a finite MoveAxis rate does reach it.
    CHECK_NOTHROW(driver->move_axis(0, 1.0));
    CHECK(motion_commands_sent() == 1);
    driver->set_connected(false);
}

// open-astro#727: a TheSkyX that accepts the TCP connect but never answers the handshake
// must not leave the shared wrapper connected, or every later connect is refused.
TEST_CASE("Bisque Telescope Driver - handshake timeout releases the wrapper", "[bisque][telescope][unit][connect]") {
    alpacacore::test::FakeMountServer silent([](const std::string&) { return std::string(); });
    alpacacore::test::FakeMountServer healthy(answer_one);
    REQUIRE(silent.ok());
    REQUIRE(healthy.ok());

    auto stuck = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(silent.port()));
    CHECK_THROWS_AS(stuck->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(stuck->get_connected());

    auto retry = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(healthy.port()));
    CHECK_NOTHROW(retry->set_connected(true));
    CHECK(retry->get_connected());
    retry->set_connected(false);
}

TEST_CASE("Bisque Telescope Driver - async handshake timeout releases the wrapper",
          "[bisque][telescope][unit][connect]") {
    alpacacore::test::FakeMountServer silent([](const std::string&) { return std::string(); });
    alpacacore::test::FakeMountServer healthy(answer_one);
    REQUIRE(silent.ok());
    REQUIRE(healthy.ok());

    auto stuck = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(silent.port()));
    stuck->connect();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (stuck->get_connecting() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(stuck->get_connecting());
    CHECK_FALSE(stuck->get_connected());
    CHECK(stuck->get_last_connect_error().find("TheSkyX") != std::string::npos);

    auto retry = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(healthy.port()));
    CHECK_NOTHROW(retry->set_connected(true));
    CHECK(retry->get_connected());
    retry->set_connected(false);
}
