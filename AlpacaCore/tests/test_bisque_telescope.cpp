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
#include <alpacacore/util/pointing.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_bisque_mount.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

using alpacacore::DeviceType;

namespace {

using alpacacore::test::require_alpaca_error;

alpacacore::vendor::bisque::ConnectionInfo loopback(int port) {
    alpacacore::vendor::bisque::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 300;
    return info;
}

std::string answer_one(const std::string&) { return "1#"; }

// The driver's status poll sleeps on the task clock (decision 0010). Cases that count TheSkyX commands or step the
// clock pause it, so no tick lands between their steps, and run exactly the ticks they need through poll_now().
void pause_poll(alpacacore::TelescopeDriver& driver) {
    alpacacore::vendor::bisque::set_status_poll_paused_for_testing(driver, true);
}
bool poll_now(alpacacore::TelescopeDriver& driver) {
    return alpacacore::vendor::bisque::poll_status_now_for_testing(driver);
}

// Steps the fake clock one second at a time, once the driver's wait is parked
// on it, until `done` holds or `max_steps` steps pass. Each step waits for the
// woken wait to settle, so no step lands between two of its waits. When no
// wait registers within 5 real seconds the driver is not sleeping on the
// clock, so the helper gives up with `done()` instead of advancing a clock
// nobody waits on (a wait that moved back to real time then fails the case).
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& done, int max_steps = 400) {
    for (int i = 0; i < max_steps; ++i) {
        bool waiting = false;
        for (int tries = 0; tries < 50 && !waiting; ++tries) {
            if (done()) return true;
            waiting = clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        }
        if (!waiting) return done();
        clock.advance(std::chrono::seconds(1));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return done();
}

template <typename T>
bool ready(std::future<T>& f) {
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// A connected Bisque driver on a FakeTaskClock over the stateful mount fake.
struct ClockedBisque {
    alpacacore::test::FakeBisqueMount mount;
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{mount.responder()};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    alpacacore::test::FakeTaskClock::clock::time_point start;

    ClockedBisque() {
        REQUIRE(server.ok());
        driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()), clock);
        REQUIRE_NOTHROW(driver->set_connected(true));
        pause_poll(*driver);
        start = clock.now();
    }
    // Virtual time that passed since the connect: it moves only when the
    // driver slept on the clock, so a wait on real time leaves it at zero.
    std::chrono::nanoseconds virtual_elapsed() const { return clock.now() - start; }
    ~ClockedBisque() { driver->set_connected(false); }
};

} // namespace

TEST_CASE("Bisque UTCDate - disconnected reads and writes are refused", "[bisque][telescope][utcdate]") {
    alpacacore::test::FakeBisqueMount mount;
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeMountServer server(mount.responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()), clock);
    const auto utc = std::chrono::system_clock::time_point(std::chrono::seconds(946684800));
    require_alpaca_error([&] { (void)driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_utc_date(utc); }, alpacacore::AlpacaError::NotConnected);
    CHECK(mount.transactions == 0);
}

TEST_CASE("Bisque UTCDate - writes are explicitly unsupported without a transaction", "[bisque][telescope][utcdate]") {
    ClockedBisque rig;
    SECTION("after a successful seed") {}
    SECTION("after a failed seed") {
        rig.driver->set_connected(false);
        rig.mount.mute = true;
        REQUIRE_NOTHROW(rig.driver->set_connected(true));
        REQUIRE(rig.driver->get_connected());
    }
    const int before = rig.mount.transactions;
    require_alpaca_error([&] { rig.driver->set_utc_date(std::chrono::system_clock::time_point{}); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(rig.mount.transactions == before);
}

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
            CHECK(std::string(ex.what()).find("RightAscension must be in range") != std::string::npos);
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

// #627: argument validation precedes the connection check (driver-lifecycle.instructions.md), so a
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
    pause_poll(*driver);
    REQUIRE(driver->get_connected());

    auto motion_commands_sent = [&]() {
        std::lock_guard<std::mutex> lock(seen_mutex);
        return std::count_if(seen.begin(), seen.end(), [](const std::string& c) {
            return c.find("SlewToRaDec") != std::string::npos || c.find("Sync(") != std::string::npos ||
                   c.find("DoCommand(9") != std::string::npos;
        });
    };

    // Count motion commands as a delta from here, so a connect handshake that ever
    // sends one does not break the absolute counts below.
    const auto baseline = motion_commands_sent();

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
    CHECK(motion_commands_sent() - baseline == 0);
    // Nothing was stored as a target either.
    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, AlpacaError::ValueNotSet);

    // The fake is live: a finite MoveAxis rate does reach it.
    const auto before_move = motion_commands_sent();
    CHECK_NOTHROW(driver->move_axis(0, 1.0));
    CHECK(motion_commands_sent() - before_move == 1);
    driver->set_connected(false);
}

TEST_CASE("Bisque Telescope Driver - all four equatorial slews are refused while Tracking is false",
          "[bisque][telescope][unit][tracking-guard]") {
    ClockedBisque rig;
    rig.mount.tracking = false;
    poll_now(*rig.driver);
    REQUIRE_FALSE(rig.driver->get_tracking());

    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&]() { rig.driver->slew_to_coordinates(5.5, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->slew_to_coordinates_async(5.5, 95.0); },
                         alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { rig.driver->slew_to_coordinates(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { rig.driver->slew_to_coordinates_async(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    rig.driver->set_target_right_ascension(5.5);
    rig.driver->set_target_declination(20.0);
    require_alpaca_error([&]() { rig.driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&]() { rig.driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // Nothing reached TheSkyX and the driver is not slewing.
    CHECK(rig.mount.slew_commands.load() == 0);
    CHECK_FALSE(rig.driver->get_slewing());

    // With Tracking on the same call goes through.
    rig.mount.tracking = true;
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(rig.mount.slew_commands.load() == 1);
}

// The TheSkyX docs do not say what IsTracking reads during a GOTO: a retarget of the driver's own slew in
// flight is not refused for it. Once TheSkyX reports the slew complete, IsTracking decides again.
TEST_CASE("Bisque Telescope Driver - a retarget of its own slew in flight is not refused for Tracking",
          "[bisque][telescope][unit][tracking-guard]") {
    ClockedBisque rig;
    rig.mount.slew_polls_needed = 1000;
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    rig.clock.advance(std::chrono::seconds(9));  // past the 8 s force window: TheSkyX decides Slewing
    rig.mount.tracking = false;
    poll_now(*rig.driver);
    REQUIRE_FALSE(rig.driver->get_tracking());

    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(6.0, 25.0));
    CHECK(rig.mount.slew_commands.load() == 2);

    // The slew lands with no Slewing read in between: the next slew is refused.
    rig.clock.advance(std::chrono::seconds(9));
    rig.mount.slewing = false;
    require_alpaca_error([&]() { rig.driver->slew_to_coordinates_async(6.5, 25.0); },
                         alpacacore::AlpacaError::InvalidOperation);
    CHECK(rig.mount.slew_commands.load() == 2);
}

TEST_CASE("Bisque async slew - completion poll errors are surfaced and valid replies are preserved",
          "[bisque][telescope][async][slewfailure]") {
    auto completion_state = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([completion_state](const std::string& command) {
        if (command.find("ConnectAndDoNotUnpark") != std::string::npos) return "1#";
        if (command.find("GetRaDec") != std::string::npos) return "|No error. Error = 0.5.5,20.0#";
        if (command.find("GetAzAlt") != std::string::npos) return "|No error. Error = 0.100.0,45.0#";
        if (command.find("IsParked") != std::string::npos) return "|No error. Error = 0.false#";
        if (command.find("IsTracking") != std::string::npos)
            return "|No error. Error = 0.1#";  // slews need Tracking (#1274)
        if (command.find("IsSlewComplete") != std::string::npos) {
            switch (completion_state->load()) {
                case 0:
                    return "|No error. Error = 1.IsSlewComplete unavailable#";
                case 1:
                    return "|No error. Error = 0.0junk#";
                case 2:
                    return "|No error. Error = 0.0#";
                default:
                    return "|No error. Error = 0.2#";  // TheSkyX documents non-zero as complete.
            }
        }
        return "|No error. Error = 0.OK#";
    });
    REQUIRE(server.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()), clock);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));

    // The driver deliberately holds Slewing=true for 8 task-clock seconds after dispatch.
    clock.advance(std::chrono::seconds(8));

    // Slewing never asks TheSkyX: the poll does, and the getter serves what it found (decision 0010).
    poll_now(*driver);
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Expected TheSkyX completion error");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("TheSkyX error") != std::string::npos);
    }

    completion_state->store(1);
    poll_now(*driver);
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Expected malformed completion error");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Failed to parse IsSlewComplete") != std::string::npos);
    }

    completion_state->store(2);
    poll_now(*driver);
    CHECK(driver->get_slewing());
    completion_state->store(3);
    poll_now(*driver);
    CHECK_FALSE(driver->get_slewing());
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

// Issue #772 (CC-19): a reply that lands after its request timed out must not
// be read as the answer to the next request, or every later reply stays one
// behind. Each query carries its number; the fake answers query 1 late.
TEST_CASE("Bisque protocol wrapper - a reply that arrives after its timeout is not given to the next command",
          "[bisque][telescope][unit][resync]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) -> std::string {
        const auto at = command.find("Out = 'R");
        if (at == std::string::npos) {
            return "1#";
        }
        const std::string id(1, command[at + 8]);
        if (id == "1") {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));  // past the 300 ms timeout
        }
        return "|No error. Error = 0.R" + id + "#";
    });
    REQUIRE(server.ok());
    auto& wrapper = alpacacore::vendor::bisque::BisqueProtocolWrapper::instance();
    REQUIRE(wrapper.connect(loopback(server.port())));
    struct Disconnect {
        alpacacore::vendor::bisque::BisqueProtocolWrapper& w;
        ~Disconnect() { w.disconnect(); }
    } disconnect_at_exit{wrapper};

    CHECK_THROWS_AS(wrapper.send_command("Out = 'R1#';", 0), alpacacore::AlpacaException);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the late reply is now in the socket
    CHECK(wrapper.send_command("Out = 'R2#';", 0) == "R2");
    CHECK(wrapper.send_command("Out = 'R3#';", 0) == "R3");
}

TEST_CASE("Bisque PulseGuide - async status remains readable and negative duration is InvalidValue",
          "[bisque][telescope][pulseguiding]") {
    auto disconnected =
        alpacacore::vendor::bisque::create_bisque_telescope(0, alpacacore::vendor::bisque::ConnectionInfo{});
    require_alpaca_error([&] { disconnected->pulse_guide(2, -1000); }, alpacacore::AlpacaError::InvalidValue);

    struct GuideState {
        std::atomic<bool> entered{false};
        std::atomic<int> calls{0};
        std::chrono::milliseconds delay{900};
    };
    auto state = std::make_shared<GuideState>();
    alpacacore::test::FakeMountServer server([state](const std::string& command) {
        if (command.find("ConnectAndDoNotUnpark") != std::string::npos) return "1#";
        if (command.find("GetRaDec") != std::string::npos) return "|No error. Error = 0.5.5,20.0#";
        if (command.find("GetAzAlt") != std::string::npos) return "|No error. Error = 0.100.0,45.0#";
        if (command.find("IsParked") != std::string::npos) return "|No error. Error = 0.false#";
        if (command.find("IsTracking") != std::string::npos)
            return "|No error. Error = 0.1#";  // slews need Tracking (#1274)
        if (command.find("MoveTelescope") != std::string::npos) {
            state->entered.store(true);
            state->calls.fetch_add(1);
            std::this_thread::sleep_for(state->delay);
        }
        return "|No error. Error = 0.OK#";
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    const auto start = std::chrono::steady_clock::now();
    REQUIRE_NOTHROW(driver->pulse_guide(2, 1000));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state->entered.load() && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(state->entered.load());

    const auto read_start = std::chrono::steady_clock::now();
    CHECK(driver->get_is_pulse_guiding());
    CHECK(std::chrono::steady_clock::now() - read_start < std::chrono::milliseconds(100));
    require_alpaca_error([&] { driver->pulse_guide(0, 1000); }, alpacacore::AlpacaError::InvalidOperation);
    const auto finished_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (driver->get_is_pulse_guiding() && std::chrono::steady_clock::now() < finished_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(state->calls.load() == 1);

    driver->pulse_guide(2, 1000);
    const auto second_guide_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (state->calls.load() < 2 && std::chrono::steady_clock::now() < second_guide_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(state->calls.load() == 2);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

namespace {

// Serves the Bisque handshake and position polls; MoveTelescope answers with
// `guide_reply` after `guide_delay`.
alpacacore::test::FakeMountServer::Responder bisque_guide_responder(std::chrono::milliseconds guide_delay,
                                                                    std::string guide_reply) {
    return [guide_delay, guide_reply](const std::string& command) -> std::string {
        if (command.find("ConnectAndDoNotUnpark") != std::string::npos) return "1#";
        if (command.find("GetRaDec") != std::string::npos) return "|No error. Error = 0.5.5,20.0#";
        if (command.find("GetAzAlt") != std::string::npos) return "|No error. Error = 0.100.0,45.0#";
        if (command.find("IsParked") != std::string::npos) return "|No error. Error = 0.false#";
        if (command.find("IsTracking") != std::string::npos)
            return "|No error. Error = 0.1#";  // slews need Tracking (#1274)
        if (command.find("MoveTelescope") != std::string::npos) {
            std::this_thread::sleep_for(guide_delay);
            return guide_reply;
        }
        return "|No error. Error = 0.OK#";
    };
}

// Polls IsPulseGuiding until it reports false or throws; returns the thrown
// AlpacaError code, or 0 when the guide finished cleanly before `deadline`,
// or -1 when it was still guiding at `deadline`.
int wait_for_pulse_guide_end(alpacacore::TelescopeDriver& driver, std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            if (!driver.get_is_pulse_guiding()) return 0;
        } catch (const alpacacore::AlpacaException& ex) {
            return ex.error_code();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

}  // namespace

TEST_CASE("Bisque PulseGuide - a hung DirectGuide fails within duration plus margin",
          "[bisque][telescope][pulseguiding]") {
    // 200 ms pulse: the response bound is 200 ms + 1 s margin. TheSkyX stays
    // silent for 2 s, so the guide must be reported failed well before then.
    alpacacore::test::FakeMountServer server(
        bisque_guide_responder(std::chrono::milliseconds(2000), "|No error. Error = 0.OK#"));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    const auto start = std::chrono::steady_clock::now();
    REQUIRE_NOTHROW(driver->pulse_guide(2, 200));
    const int outcome = wait_for_pulse_guide_end(*driver, start + std::chrono::milliseconds(1800));
    CHECK(outcome == alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("Bisque PulseGuide - a failed DirectGuide is reported until the next guide",
          "[bisque][telescope][pulseguiding]") {
    auto fail_next = std::make_shared<std::atomic<bool>>(true);
    alpacacore::test::FakeMountServer server([fail_next](const std::string& command) -> std::string {
        if (command.find("MoveTelescope") != std::string::npos) {
            return fail_next->exchange(false) ? "|No error. Error = 0.TypeError: Telescope not connected#"
                                              : "|No error. Error = 0.OK#";
        }
        return bisque_guide_responder(std::chrono::milliseconds(0), "")(command);
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->pulse_guide(0, 100));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    CHECK(wait_for_pulse_guide_end(*driver, deadline) == alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&] { (void)driver->get_is_pulse_guiding(); }, alpacacore::AlpacaError::DriverException);

    REQUIRE_NOTHROW(driver->pulse_guide(0, 100));
    CHECK(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(2)) == 0);
    driver->set_connected(false);
}

TEST_CASE("Bisque PulseGuide - the maximum duration is accepted and one more is InvalidValue",
          "[bisque][telescope][pulseguiding]") {
    alpacacore::test::FakeMountServer server(
        bisque_guide_responder(std::chrono::milliseconds(50), "|No error. Error = 0.OK#"));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    constexpr int kMaxMs = 30000;
    require_alpaca_error([&] { driver->pulse_guide(2, kMaxMs + 1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(2, std::numeric_limits<int>::max()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->pulse_guide(2, -1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->pulse_guide(2, kMaxMs));
    CHECK(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(2)) == 0);
    driver->set_connected(false);
}

TEST_CASE("Bisque FindHome - waits past the default response timeout for TheSkyX's home loop",
          "[bisque][telescope][findhome]") {
    // FindHome runs a JavaScript loop in TheSkyX that answers only once the
    // mount is home; its response bound is 60 s, not the 300 ms default here.
    alpacacore::test::FakeMountServer server([](const std::string& command) -> std::string {
        if (command.find("FindHome") != std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return "|No error. Error = 0.OK#";
        }
        return bisque_guide_responder(std::chrono::milliseconds(0), "")(command);
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->find_home());
    CHECK(driver->get_at_home());
    driver->set_connected(false);
}

TEST_CASE("Bisque AbortSlew - a getter does not stall behind a pending pulse guide",
          "[bisque][telescope][pulseguiding]") {
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto base = bisque_guide_responder(std::chrono::milliseconds(1500), "|No error. Error = 0.OK#");
    alpacacore::test::FakeMountServer server([entered, base](const std::string& command) {
        if (command.find("MoveTelescope") != std::string::npos) entered->store(true);
        return base(command);
    });
    REQUIRE(server.ok());
    auto info = loopback(server.port());
    info.response_timeout_ms = 3000;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, info);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->pulse_guide(2, 5000));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered->load() && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(entered->load());

    // AbortSlew waits for the guide reply (it cannot send on the busy socket);
    // it must do so without holding the driver mutex.
    std::thread aborter([&] { driver->abort_slew(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto read_start = std::chrono::steady_clock::now();
    CHECK_FALSE(driver->get_slewing());
    CHECK(std::chrono::steady_clock::now() - read_start < std::chrono::milliseconds(300));
    aborter.join();
    driver->set_connected(false);
}

TEST_CASE("Bisque PulseGuide - a start after AbortSlew returns at once while the superseded guide is still blocked",
          "[bisque][telescope][pulseguiding]") {
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto base = bisque_guide_responder(std::chrono::milliseconds(0), "|No error. Error = 0.OK#");
    alpacacore::test::FakeMountServer server([calls, base](const std::string& command) {
        if (command.find("MoveTelescope") != std::string::npos && calls->fetch_add(1) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        return base(command);
    });
    REQUIRE(server.ok());
    auto info = loopback(server.port());
    info.response_timeout_ms = 3000;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, info);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->pulse_guide(2, 5000));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (calls->load() == 0 && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(calls->load() == 1);

    std::thread aborter([&] { driver->abort_slew(); });
    // AbortSlew clears IsPulseGuiding before it waits on the wrapper; wait for that state, not for time.
    REQUIRE(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(1)) == 0);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_NOTHROW(driver->pulse_guide(0, 100));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300));
    aborter.join();
    // The superseded guide's reply must not clear or fail the new guide's state.
    CHECK(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(4)) == 0);
    CHECK(calls->load() == 2);
    driver->set_connected(false);
}

TEST_CASE("Bisque PulseGuide - a failed guide's error is dropped by disconnect and reconnect",
          "[bisque][telescope][pulseguiding]") {
    alpacacore::test::FakeMountServer server([](const std::string& command) -> std::string {
        if (command.find("MoveTelescope") != std::string::npos) {
            return "|No error. Error = 0.TypeError: Telescope not connected#";
        }
        return bisque_guide_responder(std::chrono::milliseconds(0), "")(command);
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()));
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->pulse_guide(0, 100));
    CHECK(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(2)) ==
          alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("Bisque PulseGuide - a short pulse keeps the configured response timeout",
          "[bisque][telescope][pulseguiding]") {
    // MoveTelescope runs at TheSkyX's fixed speed, not the requested duration,
    // so a 100 ms pulse may take longer than 100 ms + margin; the bound must
    // never drop below the configured response timeout (3000 ms here).
    alpacacore::test::FakeMountServer server(
        bisque_guide_responder(std::chrono::milliseconds(1500), "|No error. Error = 0.OK#"));
    REQUIRE(server.ok());
    auto info = loopback(server.port());
    info.response_timeout_ms = 3000;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, info);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    REQUIRE_NOTHROW(driver->pulse_guide(2, 100));
    CHECK(wait_for_pulse_guide_end(*driver, std::chrono::steady_clock::now() + std::chrono::seconds(4)) == 0);
    driver->set_connected(false);
}

TEST_CASE("Bisque Telescope Driver - out-of-range guide rates are InvalidValue and leave the rate unchanged (#775)",
          "[bisque][telescope][unit][guiderate]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);

    constexpr double kSiderealDegPerSec = 15.0411 / 3600.0;
    const auto before = driver->get_guide_rate();

    SECTION("negative or above one sidereal rate, either axis") {
        require_alpaca_error([&]() { driver->set_guide_rate({-5.0, 0.002}); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_guide_rate({0.002, 99999.0}); },
                             alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_guide_rate({0.002, -0.001}); }, alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&]() { driver->set_guide_rate({kSiderealDegPerSec * 1.01, 0.002}); },
                             alpacacore::AlpacaError::InvalidValue);
        CHECK(driver->get_guide_rate().ra == before.ra);
        CHECK(driver->get_guide_rate().dec == before.dec);
    }
    SECTION("the boundaries 0 and 1x sidereal are accepted") {
        REQUIRE_NOTHROW(driver->set_guide_rate({0.0, kSiderealDegPerSec}));
        CHECK(driver->get_guide_rate().ra == 0.0);
        CHECK(driver->get_guide_rate().dec == kSiderealDegPerSec);
    }
}

// open-astro#1112: the synchronous slew wait used to swallow every poll error and
// report a persistent TheSkyX fault as a 120 s timeout.
TEST_CASE("Bisque sync slew - a transient poll failure is tolerated and a persistent one is named",
          "[bisque][telescope][slewfailure]") {
    auto fail_polls = std::make_shared<std::atomic<int>>(1);  // failures still to serve
    auto polls = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([fail_polls, polls](const std::string& command) {
        if (command.find("ConnectAndDoNotUnpark") != std::string::npos) return "1#";
        if (command.find("GetRaDec") != std::string::npos) return "|No error. Error = 0.5.5,20.0#";
        if (command.find("GetAzAlt") != std::string::npos) return "|No error. Error = 0.100.0,45.0#";
        if (command.find("IsParked") != std::string::npos) return "|No error. Error = 0.false#";
        if (command.find("IsTracking") != std::string::npos)
            return "|No error. Error = 0.1#";  // slews need Tracking (#1274)
        if (command.find("IsSlewComplete") != std::string::npos) {
            polls->fetch_add(1);
            if (fail_polls->load() != 0) {
                if (fail_polls->load() > 0) fail_polls->fetch_sub(1);
                return "|No error. Error = 1.IsSlewComplete unavailable#";
            }
            return "|No error. Error = 0.1#";
        }
        return "|No error. Error = 0.OK#";
    });
    REQUIRE(server.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()), clock);
    REQUIRE_NOTHROW(driver->set_connected(true));
    pause_poll(*driver);

    // One failure, then success: the slew completes after one 1 s poll sleep on the clock.
    const auto start = clock.now();
    auto first = std::async(std::launch::async, [&] { driver->slew_to_coordinates(5.5, 20.0); });
    CHECK(advance_until(clock, [&] { return ready(first); }));
    CHECK_NOTHROW(first.get());
    CHECK(polls->load() == 2);
    CHECK(clock.now() - start == std::chrono::seconds(1));

    // Persistent failure: the error names the poll failure, not the timeout, and
    // the slew is not reported complete.
    fail_polls->store(-1);
    polls->store(0);
    const auto second_start = clock.now();
    auto second = std::async(std::launch::async, [&] { driver->slew_to_coordinates(5.5, 20.0); });
    CHECK(advance_until(clock, [&] { return ready(second); }));
    try {
        second.get();
        FAIL("Expected poll failure");
    } catch (const alpacacore::AlpacaException& ex) {
        const std::string what = ex.what();
        CHECK(what.find("Slew completion poll failed 3 times in a row") != std::string::npos);
        CHECK(what.find("TheSkyX error") != std::string::npos);
        CHECK(what.find("timed out") == std::string::npos);
    }
    CHECK(polls->load() == 3);
    CHECK(clock.now() - second_start == std::chrono::seconds(2));
    driver->set_connected(false);
}

// open-astro#865: every wait in the driver reads the TaskClock, so these cases
// move virtual time with advance() where they used to sleep for seconds.
TEST_CASE("Bisque park - the poll sleeps on the task clock and completes without real waiting",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 2;
    auto parked = std::async(std::launch::async, [&] { rig.driver->park(); });
    CHECK(advance_until(rig.clock, [&] { return ready(parked); }));
    REQUIRE_NOTHROW(parked.get());
    CHECK(rig.driver->get_at_park());
    // Two polls that said "not yet" (one 1 s sleep each), then the third.
    CHECK(rig.mount.park_polls.load() == 3);
    CHECK(rig.virtual_elapsed() == std::chrono::seconds(2));
}

TEST_CASE("Bisque park - a park that never completes times out after 120 virtual seconds",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 100000;
    auto parked = std::async(std::launch::async, [&] { rig.driver->park(); });
    CHECK(advance_until(rig.clock, [&] { return ready(parked); }));
    require_alpaca_error([&] { parked.get(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(rig.driver->get_at_park());
    // Polls at t=0..120 s inclusive; the 121st second trips the deadline.
    CHECK(rig.mount.park_polls.load() == 121);
}

TEST_CASE("Bisque sync slew - the poll sleep and completion run on the task clock", "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.mount.slew_polls_needed = 3;
    auto slewed = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(5.5, 20.0); });
    CHECK(advance_until(rig.clock, [&] { return ready(slewed); }));
    CHECK_NOTHROW(slewed.get());
    CHECK(rig.mount.slew_polls.load() == 4);
    CHECK(rig.virtual_elapsed() == std::chrono::seconds(3));
    CHECK_FALSE(rig.driver->get_slewing());
}

TEST_CASE("Bisque sync slew - a slew that never completes times out after 120 virtual seconds",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.mount.slew_polls_needed = 100000;
    auto slewed = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(5.5, 20.0); });
    CHECK(advance_until(rig.clock, [&] { return ready(slewed); }));
    try {
        slewed.get();
        FAIL("Expected the slew deadline");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("Slew timed out after 120 seconds") != std::string::npos);
    }
    CHECK(rig.mount.slew_polls.load() == 121);
}

TEST_CASE("Bisque sync slew - the settle time is a task-clock sleep after completion",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.driver->set_slew_settle_time(5);
    auto slewed = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(5.5, 20.0); });
    // The first poll reports completion, so the only wait is the 5 s settle.
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    rig.clock.advance(std::chrono::seconds(4));
    rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
    CHECK_FALSE(ready(slewed));
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE(slewed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK_NOTHROW(slewed.get());
    CHECK(rig.mount.slew_polls.load() == 1);
}

TEST_CASE("Bisque sync slew - Slewing is held for 8 task-clock seconds while the slew waits",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    rig.mount.slew_polls_needed = 100000;
    auto slewed = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(5.5, 20.0); });
    REQUIRE(rig.clock.wait_for_waiters(1, std::chrono::seconds(5)));
    const int polls_in_wait = rig.mount.slew_polls.load();
    // Inside the window a Slewing read is answered without asking TheSkyX.
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.slew_polls.load() == polls_in_wait);
    rig.clock.advance(std::chrono::seconds(9));
    rig.clock.wait_for_woken_settled(std::chrono::seconds(2));
    // Past the window the wait loop polls TheSkyX; the status poll leaves IsSlewComplete to it, and a Slewing
    // read never asks.
    const int before = rig.mount.slew_polls.load();
    poll_now(*rig.driver);
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.slew_polls.load() == before);
    rig.mount.slew_polls_needed = 0;
    CHECK(advance_until(rig.clock, [&] { return ready(slewed); }));
    CHECK_NOTHROW(slewed.get());
}

TEST_CASE("Bisque async slew - Slewing is held for 8 task-clock seconds without polling TheSkyX",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(rig.driver->get_slewing());
    rig.clock.advance(std::chrono::seconds(7));
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount.slew_polls.load() == 0);
    // At the 8 s mark the window is over: the next status tick asks the mount, and the read serves its answer.
    rig.clock.advance(std::chrono::seconds(1));
    poll_now(*rig.driver);
    CHECK(rig.mount.slew_polls.load() == 1);
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.mount.slew_polls.load() == 1);
}

// Decision 0010 replaces the 2 s position cache: a read is served from the snapshot however old it is (stale is
// not lost), and only a status tick asks TheSkyX again.
TEST_CASE("Bisque snapshot - a position read never asks TheSkyX, however old the frame",
          "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    const int after_connect = rig.mount.position_reads.load();
    CHECK(rig.driver->get_right_ascension() == 5.5);
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.driver->get_right_ascension() == 5.5);
    CHECK(rig.driver->get_declination() == 20.0);
    CHECK(rig.mount.position_reads.load() == after_connect);
    rig.mount.ra_hours = 6.25;
    CHECK(poll_now(*rig.driver));
    CHECK(rig.mount.position_reads.load() == after_connect + 1);
    CHECK(rig.driver->get_right_ascension() == 6.25);
}

TEST_CASE("Bisque snapshot - the alt-az read is served from the frame too", "[bisque][telescope][taskclock]") {
    ClockedBisque rig;
    const int after_connect = rig.mount.altaz_reads.load();
    static_cast<void>(rig.driver->get_azimuth());
    rig.clock.advance(std::chrono::seconds(10));
    CHECK(rig.driver->get_altitude() == 45.0);
    CHECK(rig.driver->get_azimuth() == 100.0);
    CHECK(rig.mount.altaz_reads.load() == after_connect);
    CHECK(poll_now(*rig.driver));
    CHECK(rig.mount.altaz_reads.load() == after_connect + 1);
}

// #1298: the LST helper cut UTC to whole seconds. #1807: SiderealTime follows the TheSkyX clock, so the fake clock
// drives it and the check needs no sleep.
TEST_CASE("Bisque Telescope Driver - SiderealTime follows the clock to under a second", "[bisque][telescope][unit]") {
    ClockedBisque rig;
    const double first = rig.driver->get_sidereal_time();
    rig.clock.advance(std::chrono::milliseconds(300));
    double moved_s = (rig.driver->get_sidereal_time() - first) * 3600.0;
    if (moved_s < -43200.0) {
        moved_s += 86400.0;  // wrapped past 24 h between the reads
    }
    CHECK(moved_s == Catch::Approx(0.3 * 1.00273790935).margin(0.01));
}

// #1807: SiderealTime and DestinationSideOfPier use the TheSkyX computer's clock, the one UTCDate answers from.
namespace {
constexpr double kSydneyLongitude = 151.215297;
double lst_at_millis(int64_t millis) {
    return alpacacore::util::local_sidereal_hours(
        std::chrono::system_clock::time_point(std::chrono::milliseconds(millis)), kSydneyLongitude);
}
double hours_apart(double a, double b) {
    const double d = std::fmod(std::abs(a - b), 24.0);
    return std::min(d, 24.0 - d);
}
}  // namespace

TEST_CASE("Bisque SiderealTime - answers LST of the remote clock, not the bridge host clock",
          "[bisque][telescope][utcdate][sidereal]") {
    ClockedBisque rig;
    rig.driver->set_site_longitude(kSydneyLongitude);
    // The remote clock is years from the host clock, so a host-clock LST is far off.
    const int64_t remote = 946684800123;
    rig.mount.utc_millis = remote;
    REQUIRE(poll_now(*rig.driver));
    CHECK(hours_apart(rig.driver->get_sidereal_time(), lst_at_millis(remote)) < 1e-6);
    const int before = rig.mount.transactions;
    rig.clock.advance(std::chrono::seconds(90));
    CHECK(hours_apart(rig.driver->get_sidereal_time(), lst_at_millis(remote + 90000)) < 1e-6);
    CHECK(rig.mount.transactions == before);
}

TEST_CASE("Bisque SiderealTime - disconnected throws NotConnected", "[bisque][telescope][utcdate][sidereal]") {
    ClockedBisque rig;
    rig.driver->set_connected(false);
    require_alpaca_error([&] { (void)rig.driver->get_sidereal_time(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)rig.driver->get_destination_side_of_pier(5.0, 20.0); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Bisque SiderealTime - no remote UTC raises what UTCDate raises", "[bisque][telescope][utcdate][sidereal]") {
    ClockedBisque rig;
    rig.mount.use_computer_clock = false;
    REQUIRE(poll_now(*rig.driver));
    require_alpaca_error([&] { (void)rig.driver->get_utc_date(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)rig.driver->get_sidereal_time(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { (void)rig.driver->get_destination_side_of_pier(5.0, 20.0); },
                         alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("Bisque DestinationSideOfPier - the meridian follows the remote clock",
          "[bisque][telescope][utcdate][sidereal]") {
    ClockedBisque rig;
    rig.driver->set_site_longitude(kSydneyLongitude);
    // Remote clock 12 h ahead of the host: its LST is about 12 h from the host's.
    const auto host_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const int64_t remote = host_ms + 12LL * 3600 * 1000;
    rig.mount.utc_millis = remote;
    REQUIRE(poll_now(*rig.driver));
    const double lst = lst_at_millis(remote);
    const auto wrap = [](double h) { return std::fmod(h + 24.0, 24.0); };
    // Hour angle +0.5 h under the remote clock (west of the meridian, pier east); -11.5 h under the host clock.
    CHECK(rig.driver->get_destination_side_of_pier(wrap(lst - 0.5), 20.0) == 0);
    // Hour angle -0.5 h under the remote clock (east of the meridian, pier west); +11.5 h under the host clock.
    CHECK(rig.driver->get_destination_side_of_pier(wrap(lst + 0.5), 20.0) == 1);
}

// ── #1478 spec-audit items ───────────────────────────────────────────────────

TEST_CASE("Bisque Telescope Driver spec audit - DestinationSideOfPier rejects out-of-range RA and Dec",
          "[bisque][telescope][unit][spec-audit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);
    // ITelescopeV4.DestinationSideOfPier: InvalidValue for RA outside 0..24 h or Dec outside -90..90, checked
    // before the connection (ascom-contract), so a disconnected driver answers InvalidValue too.
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(25.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(-1.0, 0.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(12.0, 91.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_destination_side_of_pier(12.0, -91.0); },
                         alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("Bisque Telescope Driver spec audit - DoesRefraction is not implemented",
          "[bisque][telescope][unit][spec-audit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);
    // The driver applies no refraction correction: ITelescopeV4 has it raise on read and on write.
    require_alpaca_error([&] { (void)driver->get_does_refraction(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(true); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_does_refraction(false); }, alpacacore::AlpacaError::PropertyNotImplemented);
}

TEST_CASE("Bisque Telescope Driver spec audit - SiteElevation never set throws InvalidOperation",
          "[bisque][telescope][unit][spec-audit]") {
    alpacacore::vendor::bisque::ConnectionInfo conn;
    conn.host = "localhost";
    conn.tcp_port = 3040;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, conn);
    // ASCOM time-and-place FAQ (decision 0019): a site value never set reads as InvalidOperation, not 0 m.
    require_alpaca_error([&] { (void)driver->get_site_elevation(); }, alpacacore::AlpacaError::InvalidOperation);
    driver->set_site_elevation(42.0);
    CHECK(driver->get_site_elevation() == 42.0);
}

// #1838: park() sends Park and polls IsParked for up to 120 s before it sets parked_, so a slew in that window
// passed the parked_ check and went to the wire. A park in progress refuses a slew like a parked mount.
TEST_CASE("Bisque slew - a park in progress refuses both slew forms and sends no slew (#1838)",
          "[bisque][telescope][taskclock][park-race]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 100000;
    auto parked = std::async(std::launch::async, [&] { rig.driver->park(); });
    // A failing check must not leave the park poll running: its future would wait for it.
    const struct FinishPark {
        ClockedBisque& rig;
        std::future<void>& parked;
        ~FinishPark() {
            rig.mount.park_polls_needed = 0;
            advance_until(rig.clock, [&] { return !parked.valid() || ready(parked); });
        }
    } finish_park{rig, parked};
    REQUIRE(advance_until(rig.clock, [&] { return rig.mount.park_polls.load() >= 1; }, 5));

    require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    // A refused slew throws at once, so wait for it in real time: stepping the clock does not give the slew thread
    // time to start under load. If a slew was sent, it waits on the clock, so step it to let the case fail, not hang.
    auto sync_slew = std::async(std::launch::async, [&] { rig.driver->slew_to_coordinates(5.5, 20.0); });
    if (sync_slew.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        advance_until(rig.clock, [&] { return ready(sync_slew); }, 20);
    }
    REQUIRE(ready(sync_slew));
    require_alpaca_error([&] { sync_slew.get(); }, alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(rig.mount.slew_commands.load() == 0);

    rig.mount.park_polls_needed = 0;
    CHECK(advance_until(rig.clock, [&] { return ready(parked); }));
    REQUIRE_NOTHROW(parked.get());
}

// AbortSlew never waits for a park poll, and it ends the park window for slews.
TEST_CASE("Bisque park - AbortSlew during the park poll returns at once and a slew is allowed again (#1838)",
          "[bisque][telescope][taskclock][park-race]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 100000;
    auto parked = std::async(std::launch::async, [&] { rig.driver->park(); });
    // A failing check must not leave the park poll running: its future would wait for it.
    const struct FinishPark {
        ClockedBisque& rig;
        std::future<void>& parked;
        ~FinishPark() {
            rig.mount.park_polls_needed = 0;
            advance_until(rig.clock, [&] { return !parked.valid() || ready(parked); });
        }
    } finish_park{rig, parked};
    REQUIRE(advance_until(rig.clock, [&] { return rig.mount.park_polls.load() >= 1; }, 5));

    auto aborted = std::async(std::launch::async, [&] { rig.driver->abort_slew(); });
    REQUIRE(aborted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE_NOTHROW(aborted.get());
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(rig.mount.slew_commands.load() == 1);

    rig.mount.park_polls_needed = 0;
    CHECK(advance_until(rig.clock, [&] { return ready(parked); }));
    REQUIRE_NOTHROW(parked.get());
}

// #1838: the slew gate drops mutex_ for IsTracking. A disconnect that lands in that gap must refuse the slew
// when the gate re-locks, before anything is sent.
TEST_CASE("Bisque slew - a disconnect inside the IsTracking gap refuses both slew forms (#1838)",
          "[bisque][telescope][unit][park-race]") {
    for (const bool async_form : {true, false}) {
        alpacacore::test::FakeBisqueMount mount;
        std::mutex gate_mutex;
        std::condition_variable gate_cv;
        bool tracking_entered = false;
        bool release = false;
        alpacacore::test::FakeMountServer server{[&](const std::string& command) {
            if (command.find("IsTracking") != std::string::npos) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                tracking_entered = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] { return release; });
            }
            return mount.responder()(command);
        }};
        REQUIRE(server.ok());
        alpacacore::test::FakeTaskClock clock;
        auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, loopback(server.port()), clock);
        REQUIRE_NOTHROW(driver->set_connected(true));
        pause_poll(*driver);

        auto slew = std::async(std::launch::async, [&] {
            if (async_form) {
                driver->slew_to_coordinates_async(5.5, 20.0);
            } else {
                driver->slew_to_coordinates(5.5, 20.0);
            }
        });
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return tracking_entered; }));
        }
        // The disconnect clears the driver state under mutex_ and then waits for the wire held by IsTracking.
        auto disconnect = std::async(std::launch::async, [&] { driver->set_connected(false); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (driver->get_connected() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        REQUIRE_FALSE(driver->get_connected());
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release = true;
        }
        gate_cv.notify_all();
        require_alpaca_error([&] { slew.get(); }, alpacacore::AlpacaError::NotConnected);
        disconnect.get();
        CHECK(mount.slew_commands.load() == 0);
    }
}

// Unpark ends the park window: AtPark reads false, so a slew must not be refused as parked.
TEST_CASE("Bisque park - Unpark during the park poll allows a slew again (#1838)",
          "[bisque][telescope][taskclock][park-race]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 100000;
    auto parked = std::async(std::launch::async, [&] { rig.driver->park(); });
    const struct FinishPark {
        ClockedBisque& rig;
        std::future<void>& parked;
        ~FinishPark() {
            rig.mount.park_polls_needed = 0;
            advance_until(rig.clock, [&] { return !parked.valid() || ready(parked); });
        }
    } finish_park{rig, parked};
    REQUIRE(advance_until(rig.clock, [&] { return rig.mount.park_polls.load() >= 1; }, 5));

    REQUIRE_NOTHROW(rig.driver->unpark());
    CHECK_FALSE(rig.driver->get_at_park());
    REQUIRE_NOTHROW(rig.driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(rig.mount.slew_commands.load() == 1);
}

// A Park that ends late (its 120 s timeout) must not clear the flag of a newer Park still polling.
TEST_CASE("Bisque park - a stale Park's timeout leaves a newer Park refusing slews (#1838)",
          "[bisque][telescope][taskclock][park-race]") {
    ClockedBisque rig;
    rig.mount.park_polls_needed = 100000;
    auto park1 = std::async(std::launch::async, [&] { rig.driver->park(); });
    std::future<void> park2;
    const struct FinishParks {
        ClockedBisque& rig;
        std::future<void>& park1;
        std::future<void>& park2;
        ~FinishParks() {
            rig.mount.park_polls_needed = 0;
            advance_until(rig.clock,
                          [&] { return (!park1.valid() || ready(park1)) && (!park2.valid() || ready(park2)); });
        }
    } finish_parks{rig, park1, park2};
    // The Park threads start in real time, so wait for them in real time: stepping the clock does not give a thread
    // time to start under load.
    const auto wait_real = [](const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return done();
    };
    REQUIRE(wait_real([&] { return rig.mount.park_polls.load() >= 1; }));

    REQUIRE_NOTHROW(rig.driver->abort_slew());
    // Park 2 starts 60 s into Park 1, so Park 1 times out with 60 s of Park 2's budget left.
    advance_until(rig.clock, [&] { return rig.virtual_elapsed() >= std::chrono::seconds(60); }, 60);
    REQUIRE(rig.virtual_elapsed() >= std::chrono::seconds(60));
    park2 = std::async(std::launch::async, [&] { rig.driver->park(); });
    REQUIRE(wait_real([&] { return rig.mount.park_commands.load() == 2; }));
    // Park 1 times out (120 s after its start) while Park 2, started later, still polls.
    REQUIRE(advance_until(rig.clock, [&] { return ready(park1); }, 200));
    require_alpaca_error([&] { park1.get(); }, alpacacore::AlpacaError::DriverException);
    REQUIRE_FALSE(ready(park2));

    require_alpaca_error([&] { rig.driver->slew_to_coordinates_async(5.5, 20.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(rig.mount.slew_commands.load() == 0);
}
