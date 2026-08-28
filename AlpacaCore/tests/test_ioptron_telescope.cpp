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
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include "catch2_compat.h"
#include "fake_ioptron_mount.h"

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
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
    REQUIRE(driver->get_can_set_tracking());
}

TEST_CASE("iOptron Telescope Driver - Target Range Validation", "[ioptron][telescope][unit]") {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, conn);

    // When disconnected, get and set target throw (connection required)
    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());

    REQUIRE_THROWS(driver->set_target_right_ascension(-0.1));
    REQUIRE_THROWS(driver->set_target_right_ascension(24.0));

    REQUIRE_THROWS(driver->set_target_declination(-90.1));
    REQUIRE_THROWS(driver->set_target_declination(90.1));
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

// Slew, then poll Slewing until the driver reports the GOTO complete. The
// driver holds Slewing true for its 5 s post-dispatch override before it
// consults the mount, so allow comfortably more than that.
bool slew_and_settle(alpacacore::TelescopeDriver& driver) {
    driver.slew_to_coordinates_async(12.0, 20.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!driver.get_slewing()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

}  // namespace

TEST_CASE("iOptron Telescope Driver - HAE16 EQ (0012) GOTO settle is closed by the pulse-guide trim",
          "[ioptron][telescope][unit][fake]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/12.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron HAE16 EQ");

    REQUIRE(slew_and_settle(*driver));

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
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, loopback_endpoint(mount.port()));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_name() == "iOptron HEM27");

    REQUIRE(slew_and_settle(*driver));

    // Not a gated model: the residual is reported as-is and no pulse is sent.
    CHECK(mount.count(":ZQ") == 0);
    CHECK(mount.count(":ZS") == 0);
    CHECK(std::abs(driver->get_right_ascension() - 12.0) * 15.0 * 3600.0 > 10.0);

    driver->set_connected(false);
}
#endif  // !_WIN32
