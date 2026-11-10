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
#include <alpacacore/util/link_health.h>
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
#include <memory>
#include <optional>
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

// :GLS reports a GOTO (2) in place of tracking, and the Tracking getter reads it as off: a slew then is still a
// slew with Tracking on, so the guard must accept it. Guiding (3) and a meridian flip (4) read as on since #2186
// and are not refused either. A commanded Tracking=false and a stopped mount at home (7) are refused.
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

    // Guiding (3) and a meridian flip (4) read as Tracking on (issue #2186), and are not refused either.
    int gotos = 2;
    for (const int status : {3, 4}) {
        INFO("system status " << status);
        mount.set_system_status(status);
        clock.advance(std::chrono::seconds(2));  // past the status TTL
        REQUIRE(driver->get_tracking());
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

// Issue #2186: the HAE29C reports system status 3 (auto-guiding) for the whole of a pulse, and 4 while it flips the
// meridian. Both are tracking with a correction on top (ASCOM: a pulse guide is a correction to sidereal tracking,
// and IsPulseGuiding reports the pulse itself), so Tracking must not read off during them; a client watching
// Tracking saw eleven false "tracking lost" faults in 50 minutes of guiding. A GOTO (2) and a stopped mount (0, 7)
// still read as off.
TEST_CASE("iOptron Telescope Driver - Tracking stays true while the mount reports guiding or a meridian flip",
          "[ioptron][telescope][unit][fake][pulseguide][tracking-guard]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_pulse_reports_guiding(true);
    alpacacore::test::FakeTaskClock clock;
    // With a site, so the Alt/Az refusal below is the Tracking one, not the missing-site one (decision 0019).
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
        0, loopback_endpoint(mount.port()), -33.856784, 151.215297, std::nullopt, false, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    clock.advance(std::chrono::seconds(31));  // past the connect fast-cache grace and the status TTL
    REQUIRE(driver->get_tracking());

    // A 3 s pulse: the firmware reports guiding from the ":Z" command on, and the next status read sees it.
    driver->pulse_guide(0, 3000);
    REQUIRE(driver->get_is_pulse_guiding());
    clock.advance(std::chrono::seconds(2));  // past the status TTL, inside the pulse
    CHECK(mount.count(":Z") == 1);           // the one pulse command, whichever axis letter the direction maps to
    CHECK(driver->get_tracking());
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());

    // The Alt/Az guard (#1377) reads the same value, so an Alt/Az slew is refused for Tracking while the mount
    // reports guiding, under its own name, with nothing sent.
    try {
        driver->slew_to_alt_az_async(30.0, 90.0);
        FAIL("the Alt/Az slew was not refused while the mount reports guiding");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
        CHECK(std::string(e.what()) == "SlewToAltAzAsync is not allowed while Tracking is true");
    }
    CHECK(mount.count(":MS1#") == 0);

    // Each status digit on its own, through a fresh ":GLS" read.
    const auto tracking_with_status = [&](int status) {
        INFO("system status " << status);
        mount.set_system_status(status);
        clock.advance(std::chrono::seconds(2));  // past the status TTL
        return driver->get_tracking();
    };
    CHECK(tracking_with_status(4));
    CHECK(tracking_with_status(3));
    CHECK(tracking_with_status(5));
    CHECK(tracking_with_status(1));
    CHECK_FALSE(tracking_with_status(2));
    CHECK_FALSE(tracking_with_status(0));
    CHECK_FALSE(tracking_with_status(7));
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

    // Latched: members that run the connection check refuse with the same message. The mount answers again here, but a
    // write never probes, so the latch holds until a probing getter runs (at most one probe every 5 s,
    // kFaultProbeInterval).
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

TEST_CASE("iOptron Telescope Driver - target getters refuse on a latched fault (#2075)",
          "[ioptron][telescope][unit][fake][fault]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_target_right_ascension(7.25);
    driver->set_target_declination(-30.25);

    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) {
        CHECK(contains(sync_failure(*driver), kReadFailure));
    }
    REQUIRE(contains(sync_failure(*driver), kLatched));

    // Latched: the stored target is not served, the same refusal as the other commanded-state reads.
    for (const auto& read : {std::function<void()>([&] { (void)driver->get_target_right_ascension(); }),
                             std::function<void()>([&] { (void)driver->get_target_declination(); })}) {
        try {
            read();
            FAIL("answered during the fault");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(contains(ex.what(), kLatched));
        }
    }

    // The sent stop is a good reply and clears the latch; the stored target is served again. sync_failure's
    // SyncToCoordinates(12, 20) stored its coordinates as the target before its read failed.
    mount.set_fail_reads(false);
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(driver->get_link_fault().empty());
    CHECK(driver->get_target_right_ascension() == 12.0);
    CHECK(driver->get_target_declination() == 20.0);

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

// ── Decision 0009: a pulled cable loses the link at once (issue #1723) ───────
//
// The serial cases run on the pty fake (PtyIoptronMount) with a FakeTaskClock and no site or time sync on connect,
// so the clock-sync body sends nothing and every driver wait is stepped by advance(). The fault cases run on the TCP
// fake, whose hangups stay a DriverException for the latch.
namespace {

namespace io = alpacacore::vendor::ioptron;
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::PtyIoptronMount;

io::ConnectionInfo serial_endpoint(const std::string& path) {
    io::ConnectionInfo info;
    info.type = io::ConnectionType::Serial;
    info.port_path = path;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> link_driver(const io::ConnectionInfo& info, FakeTaskClock& clock) {
    return io::create_ioptron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt,
                                                  /*sync_time_on_connect=*/false, clock);
}

std::function<bool(const std::string&)> on_command(std::string prefix) {
    return [prefix = std::move(prefix)](const std::string& cmd) { return cmd.rfind(prefix, 0) == 0; };
}

// Real-time bound for the fakes' I/O threads only; every driver wait runs on the fake clock.
bool wait_until(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Decision 0009 rule 3, and rule 5 while the reason is kept.
void require_lost(alpacacore::TelescopeDriver& driver, std::string_view reason) {
    CHECK_FALSE(driver.get_connected());
    CHECK(driver.get_device_state().empty());
    CHECK_FALSE(io::iOptronProtocolWrapper::instance().is_connected());  // the port is released
    require_alpaca_error([&] { (void)driver.get_right_ascension(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver.get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver.get_at_park(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver.get_tracking(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver.get_is_pulse_guiding(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver.abort_slew(); }, AlpacaError::NotConnected);
    CHECK(driver.get_supported_actions().empty());
    CHECK_FALSE(driver.can_action("gps_status"));
    CHECK(contains(driver.get_link_fault(), reason));
}

// Three forced position reads that fail latch the fault (#728) on the TCP fake.
void latch_fault(alpacacore::TelescopeDriver& driver, alpacacore::test::FakeIoptronMount& mount) {
    mount.set_fail_reads(true);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(contains(sync_failure(driver), kReadFailure));
    }
    REQUIRE(contains(sync_failure(driver), kLatched));
}

}  // namespace

TEST_CASE("iOptron Link - a hangup on a command's reply read loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":GEP#"));
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 20.0); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - EIO on a command write loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug();
    require_alpaca_error([&] { driver->set_tracking(false); }, AlpacaError::NotConnected);
    require_lost(*driver, "serial link lost (write)");
}

TEST_CASE("iOptron Link - EIO on a command write that waits for a reply loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    clock.advance(std::chrono::seconds(31));  // past the connect's cache grace: RightAscension reads on the wire
    mount.unplug();
    require_alpaca_error([&] { (void)driver->get_right_ascension(); }, AlpacaError::NotConnected);
    require_lost(*driver, "serial link lost (write)");
}

TEST_CASE("iOptron Link - a hangup in a Park's ack drain loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // :MP1# is a blind command: its ack is optional, but a hangup while waiting for it is not silence.
    mount.unplug_on(on_command(":MP1#"));
    require_alpaca_error([&] { driver->park(); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup on the park position read after :MP1# loses the link in that Park",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount("0036");  // HAE29C: Park reads the park position for its finalizer
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":GPC#"));
    require_alpaca_error([&] { driver->park(); }, AlpacaError::NotConnected);
    CHECK(mount.model().count(":MP1#") == 1);
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup in the wedged-park finalizer loses the link instead of serving AtPark",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount("0036");  // HAE29C: Park arms the finalizer for a mount that stays "slewing"
    mount.model().set_park_position_reply("00000000000000000#");
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->park());

    // The park slew is over but the firmware still says slewing; the next status read (past the connect's 30 s
    // cache grace) runs the finalizer, whose Alt/Az read meets the pull. The status read just before it must not be
    // served as AtPark false.
    mount.model().set_system_status(2);
    clock.advance(std::chrono::seconds(31));
    mount.unplug_on(on_command(":GAC#"));
    require_alpaca_error([&] { (void)driver->get_at_park(); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup on a client's Slewing poll loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    clock.advance(std::chrono::seconds(31));  // past the connect's cache grace: Slewing reads :GLS#
    mount.unplug_on(on_command(":GLS#"));
    require_alpaca_error([&] { (void)driver->get_slewing(); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup on a site read with a cached site loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->get_site_latitude());

    // Past the site cache's 60 s: a failed read would serve the cached site, but not after a pulled cable.
    clock.advance(std::chrono::seconds(61));
    mount.unplug_on(on_command(":GUT#"));
    require_alpaca_error([&] { (void)driver->get_site_latitude(); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup in AbortSlew, Unpark or an Alt/Az read loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    struct Pull {
        const char* command;
        std::function<void(alpacacore::TelescopeDriver&)> call;
    };
    const std::vector<Pull> pulls = {
        {":Q#", [](alpacacore::TelescopeDriver& d) { d.abort_slew(); }},
        {":MP0#", [](alpacacore::TelescopeDriver& d) { d.unpark(); }},
        {":GAC#", [](alpacacore::TelescopeDriver& d) { (void)d.get_altitude(); }},
    };
    for (const auto& pull : pulls) {
        INFO(pull.command);
        FakeTaskClock clock;
        PtyIoptronMount mount;
        auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        clock.advance(std::chrono::seconds(31));  // past the connect's cache grace
        mount.unplug_on(on_command(pull.command));
        require_alpaca_error([&] { pull.call(*driver); }, AlpacaError::NotConnected);
        REQUIRE(mount.unplugged());
        require_lost(*driver, "serial link lost (hangup)");
    }
}

TEST_CASE("iOptron Link - a pull mid-slew ends the slew body and loses the link",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":MS1#"));
    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_until([&] { return !driver->get_connected(); }));
    require_lost(*driver, "serial link lost (hangup)");
    // The body ended on its own (no hang); Disconnect reaps it and the session ends cleanly.
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("iOptron Link - a pull during the GOTO settle read loses the link, not Slewing false",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;  // HAE16 (0012): Slewing re-measures the RA after a GOTO lands
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_until([&] { return mount.model().count(":MS1#") == 1; }));
    mount.unplug_on(on_command(":GEP#"));
    // Poll Slewing through the 5 s post-dispatch override, as a client does, until the landed GOTO is re-measured.
    require_alpaca_error(
        [&] {
            const auto deadline = clock.now() + std::chrono::seconds(20);
            while (driver->get_slewing() && clock.now() < deadline) {
                clock.advance(std::chrono::milliseconds(100));
            }
        },
        AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a pull during a synchronous slew's settle reads fails the slew with NotConnected",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount("0025");  // HEM27: no settle trim, so the wait goes on to its position reads
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":GEP#"));
    auto slew = std::async(std::launch::async, [&] { driver->slew_to_coordinates(12.0, 20.0); });
    // The wait sleeps on the fake clock on the slew's own thread: step it until the slew returns.
    const auto real_bound = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (slew.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < real_bound) {
        clock.advance(std::chrono::milliseconds(100));
    }
    REQUIRE(slew.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    require_alpaca_error([&] { slew.get(); }, AlpacaError::NotConnected);
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a hangup on a pulse-guide's ack drain loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":ZS"));
    require_alpaca_error([&] { driver->pulse_guide(2, 500); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
}

TEST_CASE("iOptron Link - a loss during Connect fails it with NotConnected and keeps no reason",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);

    // The connect's position read logs and carries on when it fails; a hangup there must still fail the Connect.
    mount.unplug_on(on_command(":GEP#"));
    require_alpaca_error([&] { driver->set_connected(true); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK_FALSE(io::iOptronProtocolWrapper::instance().is_connected());
}

TEST_CASE("iOptron Link - the loss reason stays until Disconnect", "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.unplug_on(on_command(":GEP#"));
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 20.0); }, AlpacaError::NotConnected);
    clock.advance(std::chrono::seconds(60));
    CHECK(contains(driver->get_link_fault(), "serial link lost (hangup)"));
    driver->set_connected(false);  // the raw flag is still set: the teardown runs (#1289)
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
}

TEST_CASE("iOptron Link - Connect over a lost link reopens without a Disconnect, and nothing relinks on its own",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    auto first = std::make_unique<PtyIoptronMount>();
    alpacacore::test::ReplugPort port(*first);
    auto driver = link_driver(serial_endpoint(port.path.string()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    first->unplug_on(on_command(":GEP#"));
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 20.0); }, AlpacaError::NotConnected);
    REQUIRE_FALSE(driver->get_connected());

    // The mount comes back on the same by-id name. Nothing reopens it until the client connects.
    PtyIoptronMount second;
    port.replug(second);
    clock.advance(std::chrono::seconds(120));
    CHECK_FALSE(driver->get_connected());
    CHECK(second.model().commands().empty());
    CHECK(contains(driver->get_link_fault(), "serial link lost (hangup)"));

    // Connect, with no Disconnect first: Connected stays false until the connect is complete (#910).
    second.model().hold_next_reply(std::chrono::milliseconds(300));
    driver->connect();
    REQUIRE(second.model().wait_until_holding(std::chrono::seconds(5)));
    CHECK_FALSE(driver->get_connected());
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }));
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK(second.model().count(":GUT#") >= 1);  // the connect re-ran its reads on the new port
    CHECK_NOTHROW(driver->sync_to_coordinates(12.0, 20.0));
    driver->set_connected(false);
}

TEST_CASE("iOptron Link - a latched fault refuses reads and writes with Connected true",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = link_driver(loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount);

    const auto sent = mount.commands().size();
    const auto refused = [&](const std::function<void()>& call) {
        require_alpaca_error(call, AlpacaError::DriverException);
    };
    // Reads.
    refused([&] { (void)driver->get_right_ascension(); });
    refused([&] { (void)driver->get_declination(); });
    refused([&] { (void)driver->get_altitude(); });
    refused([&] { (void)driver->get_azimuth(); });
    refused([&] { (void)driver->get_at_park(); });
    refused([&] { (void)driver->get_at_home(); });
    refused([&] { (void)driver->get_slewing(); });
    refused([&] { (void)driver->get_tracking(); });
    refused([&] { (void)driver->get_tracking_rate(); });
    refused([&] { (void)driver->get_side_of_pier(); });
    refused([&] { (void)driver->get_sidereal_time(); });
    refused([&] { (void)driver->get_site_latitude(); });
    refused([&] { (void)driver->get_utc_date(); });
    refused([&] { (void)driver->get_guide_rate(); });
    refused([&] { (void)driver->get_is_pulse_guiding(); });
    // Writes: slews, park, pulse, sync and the setters.
    refused([&] { driver->slew_to_coordinates(12.0, 20.0); });
    refused([&] { driver->slew_to_coordinates_async(12.0, 20.0); });
    refused([&] { driver->slew_to_alt_az(45.0, 90.0); });
    refused([&] { driver->slew_to_alt_az_async(45.0, 90.0); });
    refused([&] { driver->slew_to_target_async(); });
    refused([&] { driver->sync_to_coordinates(12.0, 20.0); });
    refused([&] { driver->sync_to_target(); });
    refused([&] { driver->park(); });
    refused([&] { driver->unpark(); });
    refused([&] { driver->set_park(); });
    refused([&] { driver->find_home(); });
    refused([&] { driver->pulse_guide(2, 500); });
    refused([&] { driver->move_axis(0, 15.0 / 3600.0); });
    refused([&] { driver->set_tracking(true); });
    refused([&] { driver->set_tracking_rate(0); });
    refused([&] { driver->set_guide_rate({0.002, 0.002}); });
    refused([&] { driver->set_target_right_ascension(12.0); });
    refused([&] { driver->set_target_declination(20.0); });
    refused([&] { driver->set_site_latitude(40.0); });
    refused([&] { driver->set_site_longitude(-75.0); });
    refused([&] { driver->set_utc_date(std::chrono::system_clock::now()); });
    CHECK(driver->get_connected());
    CHECK(contains(driver->get_link_fault(), "Position"));
    CHECK(mount.commands().size() == sent);  // nothing went to the mount
}

TEST_CASE("iOptron Link - a fault standing past the staleness bound loses the link",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = link_driver(loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount);

    // Found by the next refused call: one second short of the bound it is still a fault.
    clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    require_alpaca_error([&] { (void)driver->get_tracking(); }, AlpacaError::DriverException);
    CHECK(driver->get_connected());
    clock.advance(std::chrono::seconds(1));
    require_alpaca_error([&] { (void)driver->get_tracking(); }, AlpacaError::NotConnected);
    require_lost(*driver, "stood for 30 s");

    // Connect over the lost link relinks over TCP too. A new fault that stands as long is not lost through the
    // lock-free IsPulseGuiding (it would wait on the wire lock): that stays a DriverException with Connected true, and
    // the next check_connected() call loses the link.
    mount.set_fail_reads(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    latch_fault(*driver, mount);
    clock.advance(alpacacore::util::kLinkStalenessBound);
    require_alpaca_error([&] { (void)driver->get_is_pulse_guiding(); }, AlpacaError::DriverException);
    CHECK(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_tracking(); }, AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    driver->set_connected(false);
}

// Decision 0009 rule 1: the next good reply clears a latched fault. While latched, the position, Alt/Az and status
// getters send one :GLS# probe per probe interval (5 s on the task clock) and refuse with nothing sent in between.
TEST_CASE("iOptron Link - a latched fault clears on the next good probe reply",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = link_driver(loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount);
    mount.set_fail_reads(false);  // the mount answers again

    // Before the interval: refused, nothing sent.
    const auto sent = mount.commands().size();
    clock.advance(std::chrono::seconds(4));
    require_alpaca_error([&] { (void)driver->get_right_ascension(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { (void)driver->get_slewing(); }, AlpacaError::DriverException);
    CHECK(mount.commands().size() == sent);

    // At the interval the read probes, the good reply clears the latch and the read returns.
    clock.advance(std::chrono::seconds(1));
    const int status_reads = mount.count(":GLS#");
    CHECK_NOTHROW(driver->get_right_ascension());
    CHECK(mount.count(":GLS#") == status_reads + 1);  // the one probe
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    CHECK_NOTHROW(driver->get_is_pulse_guiding());
    CHECK(sync_failure(*driver).empty());  // writes go out again
    driver->set_connected(false);
}

TEST_CASE("iOptron Link - a failed probe keeps the latch and failing probes lose the link at the bound",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = link_driver(loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount);  // reads keep failing

    // RightAscension: inside the connect's 30 s cache grace its position read is served from the connect's cache, so
    // only the failed probe itself can make the call throw; a probe whose failure were dropped would serve that value.
    for (int probe = 1; probe <= 5; ++probe) {  // at 5, 10, 15, 20 and 25 s
        clock.advance(std::chrono::seconds(5));
        const int status_reads = mount.count(":GLS#");
        require_alpaca_error([&] { (void)driver->get_right_ascension(); }, AlpacaError::DriverException);
        CHECK(mount.count(":GLS#") == status_reads + 1);  // one probe
        CHECK(driver->get_connected());
        CHECK_FALSE(driver->get_link_fault().empty());
        const auto sent = mount.commands().size();
        require_alpaca_error([&] { (void)driver->get_altitude(); }, AlpacaError::DriverException);
        CHECK(mount.commands().size() == sent);  // no second probe in the same interval
    }
    clock.advance(std::chrono::seconds(4));  // 29 s: still a fault
    require_alpaca_error([&] { (void)driver->get_tracking(); }, AlpacaError::DriverException);
    CHECK(driver->get_connected());
    clock.advance(std::chrono::seconds(1));  // 30 s with no good reply: lost, even though a probe is due
    require_alpaca_error([&] { (void)driver->get_tracking(); }, AlpacaError::NotConnected);
    require_lost(*driver, "stood for 30 s");
    driver->set_connected(false);
}

TEST_CASE("iOptron Link - writes refuse a latched fault with nothing sent even when a probe is due",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    auto driver = link_driver(loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount);
    mount.set_fail_reads(false);  // the mount answers again, but no write may move it on a faulted link
    clock.advance(std::chrono::seconds(10));

    const auto sent = mount.commands().size();
    const auto refused = [&](const std::function<void()>& call) {
        require_alpaca_error(call, AlpacaError::DriverException);
    };
    refused([&] { driver->slew_to_coordinates_async(12.0, 20.0); });
    refused([&] { driver->slew_to_alt_az_async(45.0, 90.0); });
    refused([&] { driver->sync_to_coordinates(12.0, 20.0); });
    refused([&] { driver->park(); });
    refused([&] { driver->unpark(); });
    refused([&] { driver->find_home(); });
    refused([&] { driver->pulse_guide(2, 500); });
    refused([&] { driver->move_axis(0, 15.0 / 3600.0); });
    refused([&] { driver->set_tracking(true); });
    refused([&] { driver->set_tracking_rate(0); });
    refused([&] { driver->set_site_latitude(40.0); });
    CHECK(mount.commands().size() == sent);
    CHECK(driver->get_connected());

    CHECK_NOTHROW(driver->get_tracking());  // a probe-eligible read clears it
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

TEST_CASE("iOptron Link - a hangup during a latched-fault probe loses the link at once",
          "[ioptron][telescope][unit][fake][link-lost]") {
    FakeTaskClock clock;
    PtyIoptronMount mount;
    auto driver = link_driver(serial_endpoint(mount.slave_path()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    latch_fault(*driver, mount.model());

    clock.advance(std::chrono::seconds(5));
    mount.unplug_on(on_command(":GLS#"));
    require_alpaca_error([&] { (void)driver->get_at_park(); }, AlpacaError::NotConnected);
    REQUIRE(mount.unplugged());
    require_lost(*driver, "serial link lost (hangup)");
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

    // The pulse duration has passed but the driver's 500 ms completion delay has not: the firmware is still
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
    clock.advance(home_at + std::chrono::milliseconds(5400) - clock.now());  // inside 3 s + the 500 ms delay
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// ── Decision 0022: the slew and pulse windows are util::HoldWindows on the task clock (issue #1831) ─────────
//
// HEM27 (0025): no GOTO trim, so Slewing past the window is the mount's status alone (1, tracking, unless set).
namespace {

namespace io = alpacacore::vendor::ioptron;
using alpacacore::test::FakeIoptronMount;
using alpacacore::test::FakeTaskClock;

std::unique_ptr<alpacacore::TelescopeDriver> hold_driver(FakeIoptronMount& mount, FakeTaskClock& clock) {
    auto driver = io::create_ioptron_telescope_with_site(0, loopback_endpoint(mount.port()), std::nullopt, std::nullopt,
                                                         std::nullopt,
                                                         /*sync_time_on_connect=*/false, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

// Holds the async slew body before it takes mutex_ until release() (or destruction), so a case can act in the
// gap between SlewToCoordinatesAsync returning and its :MS1# going out.
struct HeldSlewBody {
    std::promise<void> released;
    std::shared_future<void> gate = released.get_future().share();
    bool done = false;
    std::function<void()> hook() {
        return [gate = gate] { gate.wait(); };
    }
    void release() {
        if (!done) released.set_value();
        done = true;
    }
    ~HeldSlewBody() { release(); }
};

// Past the driver's 30 s post-connect cache, so reads after a 1 s cache age reach the mount.
void pass_connect_cache(FakeTaskClock& clock) { clock.advance(std::chrono::seconds(31)); }

constexpr double kGuideRateDegPerSec = 0.5 * 360.0 / 86164.0905;  // the driver's default guide rate

}  // namespace

// Rule 2: armed before the command goes out. The body has not taken mutex_ yet, so nothing has been sent; Slewing
// is true from the moment the call returns.
TEST_CASE("iOptron hold window - Slewing is true as soon as SlewToCoordinatesAsync returns",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    HeldSlewBody body;  // released before the driver joins its slot
    driver = io::create_ioptron_telescope_with_slew_worker_hook(0, loopback_endpoint(mount.port()), body.hook(),
                                                                std::nullopt, std::nullopt, {}, {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(12.0, 20.0);
    CHECK(mount.count(":MS1#") == 0);
    CHECK(driver->get_slewing());
    body.release();
    REQUIRE(wait_for_goto_dispatch(mount));
    CHECK(driver->get_slewing());
    driver->abort_slew();
    driver->set_connected(false);
}

// The Alt/Az form arms the same window in its initiator (the mount is stopped: an Alt/Az slew needs Tracking off).
TEST_CASE("iOptron hold window - Slewing is true as soon as SlewToAltAzAsync returns",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(0);
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    HeldSlewBody body;  // released before the driver joins its slot
    driver = io::create_ioptron_telescope_with_slew_worker_hook(0, loopback_endpoint(mount.port()), body.hook(),
                                                                std::nullopt, std::nullopt, {}, {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_alt_az_async(45.0, 180.0);
    CHECK(mount.count(":MS1#") == 0);
    CHECK(driver->get_slewing());
    body.release();
    REQUIRE(wait_for_goto_dispatch(mount));
    driver->abort_slew();
    driver->set_connected(false);
}

// Rule 2: the dispatch arms the window again before its :MS1#, so it runs 5 s from the GOTO, not from an
// initiator whose body waited for mutex_.
TEST_CASE("iOptron hold window - the GOTO's window runs 5 s from its :MS1#",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    HeldSlewBody body;  // released before the driver joins its slot
    driver = io::create_ioptron_telescope_with_slew_worker_hook(0, loopback_endpoint(mount.port()), body.hook(),
                                                                std::nullopt, std::nullopt, {}, {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(12.0, 20.0);
    clock.advance(std::chrono::seconds(4));
    body.release();
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::milliseconds(4900));
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(100));
    CHECK_FALSE(driver->get_slewing());  // the mount reports tracking
    driver->set_connected(false);
}

TEST_CASE("iOptron hold window - AbortSlew inside the hold clears Slewing at once",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    REQUIRE(driver->get_slewing());
    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(500));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("iOptron hold window - a reconnect inside the hold leaves Slewing false",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    REQUIRE(driver->get_slewing());
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// MoveAxis starts no GOTO: it ends the window of the GOTO it supersedes, and Slewing then follows the axis flags
// and the mount.
TEST_CASE("iOptron hold window - MoveAxis inside a GOTO's hold ends the window",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    REQUIRE(driver->get_slewing());
    driver->move_axis(0, 15.0 / 3600.0);
    CHECK(driver->get_slewing());
    driver->move_axis(0, 0.0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Rule 2: a failed dispatch clears the window. The link is reset, so the synchronous slew's :SRA# write fails;
// Slewing reads the status cached at connect instead of the 5 s window.
TEST_CASE("iOptron hold window - a synchronous SlewToCoordinates whose dispatch fails leaves Slewing false",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    mount.reset_link();
    CHECK_THROWS(driver->slew_to_coordinates(12.0, 20.0));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// A synchronous slew whose dispatch throws also ends slew_in_progress_. Left true, it would stop the #843 mask: the
// mount's system status 2 during a guide pulse would read as Slewing. The status 2 is the one cached at connect, and
// the link is reset after the pulse, so no read goes to it.
TEST_CASE("iOptron hold window - a failed synchronous slew does not unmask a guide pulse",
          "[ioptron][telescope][unit][fake][hold-window][pulseguide]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(2);
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->pulse_guide(0, 2000);
    REQUIRE(driver->get_is_pulse_guiding());
    mount.reset_link();
    CHECK_THROWS(driver->slew_to_coordinates(12.0, 20.0));
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Rule 2: a Park whose :MP1# cannot be written takes its window back.
TEST_CASE("iOptron hold window - a Park whose command fails leaves Slewing false",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    mount.reset_link();
    require_alpaca_error([&] { driver->park(); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Hold lengths are unchanged: FindHome and Park 5 s, Unpark 2 s, each from its command. Past the 1 s status cache
// the mount reports tracking, so only the window keeps Slewing true.
TEST_CASE("iOptron hold window - FindHome, Park and Unpark hold Slewing for 5 s, 5 s and 2 s",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);
    pass_connect_cache(clock);

    driver->find_home();
    REQUIRE(mount.count(":MH#") == 1);
    clock.advance(std::chrono::milliseconds(4900));
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(100));
    CHECK_FALSE(driver->get_slewing());

    driver->park();
    REQUIRE(mount.count(":MP1#") == 1);
    clock.advance(std::chrono::milliseconds(4900));
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(100));
    CHECK_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE(mount.count(":MP0#") == 1);
    clock.advance(std::chrono::milliseconds(1900));
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(100));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// The GOTO trim (HAE16 EQ, a 12 arcsec settle) holds Slewing for its pulse plus 500 ms: about 1.6 s at the
// default guide rate, so true at 2.0 s and false at 2.2 s.
TEST_CASE("iOptron hold window - the GOTO trim holds Slewing for its pulse",
          "[ioptron][telescope][unit][fake][hold-window]") {
    FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::seconds(5));
    CHECK(driver->get_slewing());  // the trim goes out
    REQUIRE(mount.count(":ZQ") == 1);
    clock.advance(std::chrono::milliseconds(2000));
    CHECK(driver->get_slewing());
    clock.advance(std::chrono::milliseconds(200));
    CHECK_FALSE(driver->get_slewing());
    CHECK(mount.count(":ZQ") == 1);
    driver->set_connected(false);
}

// Rule 4: a North pulse of 2 s. IsPulseGuiding is true until 2 s plus the 500 ms completion delay of virtual time
// (#2001); RA is held at its last read until 2 s + 0.5 s + 2 s; the first Dec read serves the predicted position.
TEST_CASE("iOptron hold window - a pulse keeps its axis while virtual time has not reached its end",
          "[ioptron][telescope][unit][fake][hold-window][pulseguide]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_position(10.0, 20.0);
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);
    pass_connect_cache(clock);
    REQUIRE(driver->get_right_ascension() == Catch::Approx(10.0));
    REQUIRE(driver->get_declination() == Catch::Approx(20.0));
    mount.set_position(11.0, 21.0);

    const auto sent = clock.now();
    driver->pulse_guide(0, 2000);
    clock.advance(std::chrono::milliseconds(1100));
    CHECK(driver->get_right_ascension() == Catch::Approx(10.0));                          // held
    CHECK(driver->get_declination() == Catch::Approx(20.0 + 2.0 * kGuideRateDegPerSec));  // predicted, once
    CHECK(driver->get_declination() == Catch::Approx(21.0));                              // then live
    clock.advance(sent + std::chrono::milliseconds(2499) - clock.now());
    CHECK(driver->get_is_pulse_guiding());
    clock.advance(std::chrono::milliseconds(1));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    clock.advance(sent + std::chrono::milliseconds(4499) - clock.now());
    CHECK(driver->get_right_ascension() == Catch::Approx(10.0));
    clock.advance(std::chrono::milliseconds(1));
    CHECK(driver->get_right_ascension() == Catch::Approx(11.0));
    driver->set_connected(false);
}

// Issue #2001: the HAE29C ends its guiding status 70 to 380 ms after a pulse's duration, so IsPulseGuiding falls
// 500 ms after it, not 1 s: a guider waiting on it loses half a second less per correction. Falsified by:
// ioptron_telescope_driver.cpp kPulseGuideCompletionDelay set back to 1000 ms.
TEST_CASE("iOptron Telescope Driver - IsPulseGuiding falls 500 ms after the pulse duration",
          "[ioptron][telescope][unit][fake][pulseguide]") {
    FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);
    pass_connect_cache(clock);
    for (const int duration : {300, 1000, 3000}) {
        INFO("pulse " << duration << " ms");
        const auto sent = clock.now();
        driver->pulse_guide(2, duration);
        clock.advance(sent + std::chrono::milliseconds(duration + 450) - clock.now());
        CHECK(driver->get_is_pulse_guiding());
        clock.advance(sent + std::chrono::milliseconds(duration + 550) - clock.now());
        CHECK_FALSE(driver->get_is_pulse_guiding());
    }
    driver->set_connected(false);
}

// Each pulse holds the axis it does not move and ends the hold a pulse on the other axis set; a zero-length pulse
// ends both holds and IsPulseGuiding.
TEST_CASE("iOptron hold window - each pulse holds the other axis and ends the earlier hold",
          "[ioptron][telescope][unit][fake][hold-window][pulseguide]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_position(10.0, 20.0);
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);
    pass_connect_cache(clock);
    REQUIRE(driver->get_right_ascension() == Catch::Approx(10.0));
    REQUIRE(driver->get_declination() == Catch::Approx(20.0));
    mount.set_position(11.0, 21.0);

    SECTION("an East pulse holds Dec, predicts RA and ends a North pulse's RA hold") {
        driver->pulse_guide(0, 2000);    // North: holds RA
        driver->pulse_guide(2, 2000);    // East: holds Dec, ends the RA hold
        mount.set_position(11.0, 21.0);  // the fake's :ZS models a GOTO trim and moved RA; the ack came after it
        clock.advance(std::chrono::milliseconds(1100));
        CHECK(driver->get_right_ascension() == Catch::Approx(10.0 + 2.0 * kGuideRateDegPerSec / 15.0));
        CHECK(driver->get_right_ascension() == Catch::Approx(11.0));
        (void)driver->get_declination();  // the North pulse's prediction, once
        CHECK(driver->get_declination() == Catch::Approx(20.0));
    }
    SECTION("a North pulse ends an East pulse's Dec hold") {
        driver->pulse_guide(2, 2000);
        driver->pulse_guide(0, 2000);
        clock.advance(std::chrono::milliseconds(1100));
        (void)driver->get_declination();      // the North pulse's prediction, once
        (void)driver->get_right_ascension();  // the East pulse's prediction, once
        CHECK(driver->get_declination() == Catch::Approx(21.0));
        CHECK(driver->get_right_ascension() == Catch::Approx(10.0));
    }
    SECTION("a zero-length pulse ends both holds and IsPulseGuiding") {
        driver->pulse_guide(0, 2000);
        driver->pulse_guide(0, 0);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        (void)driver->get_declination();  // the North pulse's prediction, once
        clock.advance(std::chrono::milliseconds(1100));
        CHECK(driver->get_right_ascension() == Catch::Approx(11.0));
        driver->pulse_guide(2, 2000);
        driver->pulse_guide(2, 0);
        (void)driver->get_right_ascension();  // the East pulse's prediction, once
        clock.advance(std::chrono::milliseconds(1100));
        CHECK(driver->get_declination() == Catch::Approx(21.0));
    }
    driver->set_connected(false);
}

// Rule 2: a PulseGuide whose command cannot be written leaves no pulse window: IsPulseGuiding is false and the next
// Dec read is the cached position, not a prediction. Inside the 30 s connect cache, so no read goes to the reset link.
TEST_CASE("iOptron hold window - a PulseGuide whose command fails leaves no pulse window",
          "[ioptron][telescope][unit][fake][hold-window][pulseguide]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_position(10.0, 20.0);
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);
    REQUIRE(driver->get_declination() == Catch::Approx(20.0));

    driver->pulse_guide(2, 3000);
    REQUIRE(driver->get_is_pulse_guiding());
    mount.reset_link();
    CHECK_THROWS(driver->pulse_guide(0, 2000));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_declination() == Catch::Approx(20.0));
    driver->set_connected(false);
}

// Rule 2: a reconnect starts with no pulse window: IsPulseGuiding is false and RA and Dec are the mount's.
TEST_CASE("iOptron hold window - a reconnect inside a pulse leaves no pulse window",
          "[ioptron][telescope][unit][fake][hold-window][pulseguide]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_position(10.0, 20.0);
    FakeTaskClock clock;
    auto driver = hold_driver(mount, clock);

    driver->pulse_guide(0, 2000);
    REQUIRE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_right_ascension() == Catch::Approx(10.0));
    CHECK(driver->get_declination() == Catch::Approx(20.0));
    driver->set_connected(false);
}

// Rule 2, the connect itself: a Connect over a lost link runs no Disconnect first (decision 0009 rule 4), and the
// new session still starts with no window.
TEST_CASE("iOptron hold window - a Connect over a lost link inside the holds leaves no window",
          "[ioptron][telescope][unit][fake][hold-window][link-lost]") {
    FakeTaskClock clock;
    auto first = std::make_unique<alpacacore::test::PtyIoptronMount>();
    first->model().set_position(10.0, 20.0);
    alpacacore::test::ReplugPort port(*first);
    auto driver = link_driver(serial_endpoint(port.path.string()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(12.0, 20.0);
    REQUIRE(wait_until([&] { return first->model().count(":MS1#") == 1; }));
    driver->pulse_guide(0, 2000);
    first->unplug_on(on_command(":GEP#"));
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 20.0); }, AlpacaError::NotConnected);
    REQUIRE_FALSE(driver->get_connected());

    alpacacore::test::PtyIoptronMount second;
    second.model().set_position(11.0, 21.0);
    port.replug(second);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(driver->get_slewing());
    CHECK_FALSE(driver->get_is_pulse_guiding());
    CHECK(driver->get_declination() == Catch::Approx(21.0));
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

    mount.set_system_status(0);               // an Alt/Az slew needs Tracking off (#1377), as ConformU does
    clock.advance(std::chrono::seconds(31));  // past the connect fast-cache grace and the status TTL
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
    mount.set_system_status(0);               // an Alt/Az slew needs Tracking off (#1377)
    clock.advance(std::chrono::seconds(31));  // past the connect fast-cache grace and the status TTL
    require_alpaca_error([&] { driver->slew_to_alt_az_async(30.0, 90.0); }, alpacacore::AlpacaError::DriverException);

    // The refused starts rolled back: the earlier failure is still the one Slewing reports, and no GOTO was sent.
    std::string after;
    REQUIRE(wait_for_slewing_error(*driver, after));
    CHECK(after == earlier);
    CHECK(mount.count(":MS1#") == gotos_before);
    driver->set_connected(false);
}

// Issue #1377 (ITelescopeV4): SlewToAltAz and SlewToAltAzAsync throw InvalidOperation while Tracking is true, before
// anything is sent. Tracking off, the slew goes out as before.
TEST_CASE("iOptron Telescope Driver - an Alt/Az slew is refused while Tracking is true",
          "[ioptron][telescope][unit][fake]") {
    for (const bool async : {false, true}) {
        CAPTURE(async);
        const char* name = async ? "SlewToAltAzAsync" : "SlewToAltAz";
        alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
        REQUIRE(mount.ok());
        alpacacore::test::FakeTaskClock clock;
        auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
            0, loopback_endpoint(mount.port()), -33.856784, 151.215297, std::nullopt, false, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        // Runs the slew on another thread and advances the fake clock until it returns (a synchronous slew waits
        // on it), so a regression that lets the slew through fails instead of hanging. Returns the call's future.
        auto run_slew = [&] {
            auto call = std::async(std::launch::async, [&] {
                if (async) {
                    driver->slew_to_alt_az_async(30.0, 90.0);
                } else {
                    driver->slew_to_alt_az(30.0, 90.0);
                }
            });
            for (int i = 0; i < 400; ++i) {
                if (call.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready) break;
                clock.advance(std::chrono::seconds(1));
            }
            return call;
        };

        // Tracking on (the fake's default status 1): refused under its own name, and no GOTO is sent.
        REQUIRE(driver->get_tracking());
        auto refused = run_slew();
        REQUIRE(refused.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        try {
            refused.get();
            FAIL("the Alt/Az slew was not refused");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(e.what()) == std::string(name) + " is not allowed while Tracking is true");
        }
        CHECK(mount.count(":MS1#") == 0);
        CHECK(mount.count(":MS2#") == 0);

        // Tracking off: the slew goes out.
        mount.set_system_status(0);
        clock.advance(std::chrono::seconds(31));  // past the connect fast-cache grace and the status TTL
        REQUIRE_FALSE(driver->get_tracking());
        auto accepted = run_slew();
        REQUIRE(accepted.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        CHECK_NOTHROW(accepted.get());
        CHECK(wait_until([&] { return mount.count(":MS") >= 1; }));  // the async GOTO goes out from the slot's thread

        // Parked (status 6): refused under its own name with InvalidWhileParked, and nothing more is sent (the
        // synchronous form used to report this as SlewToCoordinates).
        const auto gotos_before_park = mount.count(":MS");
        mount.set_system_status(6);
        clock.advance(std::chrono::seconds(31));  // past the status TTL
        REQUIRE(driver->get_at_park());           // a client's AtPark poll refreshes the cache, as it would in use
        auto parked = run_slew();
        REQUIRE(parked.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
        try {
            parked.get();
            FAIL("the Alt/Az slew of a parked mount was not refused");
        } catch (const alpacacore::AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
            CHECK(std::string(e.what()) == std::string(name) + " is not allowed while parked");
        }
        CHECK(mount.count(":MS") == gotos_before_park);
        driver->set_connected(false);
    }
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

// Issue #2063: an async GOTO that nobody polls leaves slew_in_progress_ set after the mount stopped.
TEST_CASE("iOptron Telescope Driver - Slewing stays false for a PulseGuide after an unpolled async GOTO finished",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    // The fake lands at once and reports status 0. The client never reads Slewing; time passes past the
    // dispatch override, and PulseGuide's own status refresh sees the mount stopped before the pulse starts.
    clock.advance(std::chrono::seconds(10));
    (void)driver->get_right_ascension();
    driver->pulse_guide(3, 2704);
    // The pulse makes the firmware report slewing (status 2) on the firmware that showed open-astro#843.
    mount.set_system_status(2);
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Issue #2063: MoveAxis after a GOTO that finished unpolled must not keep it as driver motion.
TEST_CASE("iOptron Telescope Driver - MoveAxis after an unpolled finished GOTO leaves no driver motion",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::seconds(10));
    (void)driver->get_right_ascension();
    driver->move_axis(0, 0.0);
    driver->pulse_guide(3, 2704);
    mount.set_system_status(2);
    CHECK(driver->get_is_pulse_guiding());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Issue #2063: the settle trim must not fire in the middle of the client's pulse after an unpolled GOTO.
TEST_CASE("iOptron Telescope Driver - no settle trim during a PulseGuide after an unpolled async GOTO",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::seconds(10));
    (void)driver->get_right_ascension();
    const auto zq_before = mount.count(":ZQ");
    driver->pulse_guide(0, 2704);
    mount.set_system_status(2);
    CHECK_FALSE(driver->get_slewing());
    CHECK(mount.count(":ZQ") == zq_before);
    driver->set_connected(false);
}

// Issue #2063: past the 30 s connect fast cache, Slewing reads the pulse's status 2 and the mask must hold.
TEST_CASE("iOptron Telescope Driver - Slewing stays false for a PulseGuide after an unpolled GOTO and a fresh status",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    clock.advance(std::chrono::seconds(31));

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::seconds(10));
    (void)driver->get_right_ascension();
    driver->pulse_guide(3, 2704);
    mount.set_system_status(2);
    clock.advance(std::chrono::milliseconds(1500));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Issue #2063: a status read while the async body waits for mutex_ sees the mount still stopped; the GOTO that goes
// out after it must clear that stop, or a PulseGuide masks the running slew.
TEST_CASE("iOptron Telescope Driver - Slewing stays true for a PulseGuide during a GOTO sent after a held body",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    HeldSlewBody body;  // released before the driver joins its slot
    driver = io::create_ioptron_telescope_with_slew_worker_hook(0, loopback_endpoint(mount.port()), body.hook(),
                                                                std::nullopt, std::nullopt, {}, {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    pass_connect_cache(clock);

    driver->slew_to_coordinates_async(13.0, 20.0);
    clock.advance(std::chrono::seconds(6));
    (void)driver->get_tracking();  // fresh status past the initiator's window: the mount has not moved yet
    body.release();
    REQUIRE(wait_for_goto_dispatch(mount));
    mount.set_system_status(2);
    clock.advance(std::chrono::seconds(6));
    CHECK(driver->get_slewing());
    driver->pulse_guide(3, 2704);
    CHECK(driver->get_slewing());
    driver->abort_slew();
    driver->set_connected(false);
}

// Issue #2063: the settle trim is part of the GOTO. The stop seen before the trim must not outlive it, or a client
// pulse during the trimmed GOTO masks Slewing.
TEST_CASE("iOptron Telescope Driver - Slewing stays true for a PulseGuide after a settle trim",
          "[ioptron][telescope][unit][fake][pulseguide][repro2063]") {
    FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    FakeTaskClock clock;
    auto driver = io::create_ioptron_telescope(0, loopback_endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    pass_connect_cache(clock);

    driver->slew_to_coordinates_async(13.0, 20.0);
    REQUIRE(wait_for_goto_dispatch(mount));
    clock.advance(std::chrono::seconds(6));
    // Fresh status past the dispatch window: the mount stopped 12" off, so the trim pulse goes out.
    CHECK(driver->get_slewing());
    REQUIRE(mount.count(":ZQ") == 1);
    driver->pulse_guide(3, 3000);
    mount.set_system_status(2);
    clock.advance(std::chrono::milliseconds(2100));  // past the trim hold, inside the client's pulse window
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}
