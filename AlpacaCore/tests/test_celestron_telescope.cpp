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
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "catch2_compat.h"

#ifndef _WIN32
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#endif

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

} // namespace

TEST_CASE("Celestron Telescope Driver - Defaults", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(driver->get_name() == "Celestron NexStar Telescope");

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
    REQUIRE_FALSE(driver->get_can_pulse_guide());
    REQUIRE_FALSE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_set_tracking());
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
}

TEST_CASE("Celestron Telescope Driver - Device metadata", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(3, conn);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Celestron NexStar Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron NexStar Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "Celestron_3");
}

TEST_CASE("Celestron Telescope Driver - Disconnected Behavior", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

TEST_CASE("Celestron Telescope Driver - Unsupported actions", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Celestron Telescope Driver - Target Range Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

TEST_CASE("Celestron Telescope Driver - Axis Rate Ranges", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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
}

TEST_CASE("Celestron Telescope Driver - Target Coordinate Persistence", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

TEST_CASE("Celestron Telescope Driver - Site Property Validation", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Celestron Telescope Driver - Telescope Properties", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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
}

TEST_CASE("Celestron Telescope Driver - ASCOM Error Codes", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

    // TODO: Celestron check_connected() throws DriverException (0x500) instead of
    // NotConnected (0x407). Fix the driver, then change these to AlpacaError::NotConnected.
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::DriverException);

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
TEST_CASE("Celestron Telescope Driver - the two target properties are independent", "[celestron][telescope][unit]") {
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);

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

#ifndef _WIN32

// ── The client-clock disagreement warning (#409) ─────────────────────────────

TEST_CASE("Celestron Telescope Driver - a far-off client UTCDate is logged once per connection on a disciplined host",
          "[celestron][telescope][unit]") {
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
            if (level == alpacacore::logging::LogLevel::Warn && component == "Celestron" &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });

    // NexStar "H" (set time) ignores the reply; a position-pair sized reply
    // with the trailing '#' keeps every fixed-length connect read from
    // waiting out its timeout (see test_celestron_concurrency_stress.cpp).
    alpacacore::test::FakeMountServer server([](const std::string&) { return std::string("00000000,00000000#"); });
    REQUIRE(server.ok());
    alpacacore::vendor::celestron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::celestron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = server.port();
    // 500 ms, not the stress files' 50 ms: the "H" reply is read one byte at
    // a time against the whole-response budget, and 50 ms is not enough for
    // the 18-byte canned reply even on loopback (timed out every run).
    conn.response_timeout_ms = 500;
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, conn);
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

#endif  // _WIN32
