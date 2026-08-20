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
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>
#include <alpacacore/version.h>

#include <functional>

#include "catch2_compat.h"

using alpacacore::AlignmentMode;
using alpacacore::DeviceType;
using alpacacore::EquatorialSystem;
namespace sw = alpacacore::vendor::skywatcher;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int device_number = 0) {
    sw::ConnectionInfo conn;
    conn.type = sw::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    return sw::create_skywatcher_telescope(device_number, conn);
}

} // namespace

TEST_CASE("SkyWatcher Telescope Driver - Defaults", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_device_type() == DeviceType::Telescope);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Sky-Watcher Wave Mount");

    REQUIRE(driver->get_can_slew());
    REQUIRE(driver->get_can_slew_async());
    REQUIRE_FALSE(driver->get_can_slew_alt_az());
    REQUIRE_FALSE(driver->get_can_slew_alt_az_async());
    REQUIRE(driver->get_can_sync());
    REQUIRE_FALSE(driver->get_can_sync_alt_az());
    REQUIRE_FALSE(driver->get_can_find_home());
    REQUIRE(driver->get_can_park());
    REQUIRE(driver->get_can_unpark());
    REQUIRE(driver->get_can_set_park());
    REQUIRE(driver->get_can_pulse_guide());
    REQUIRE(driver->get_can_set_guide_rates());
    REQUIRE(driver->get_can_set_tracking());
    REQUIRE_FALSE(driver->get_can_set_pier_side());
    REQUIRE_FALSE(driver->get_can_set_declination_rate());
    REQUIRE_FALSE(driver->get_can_set_right_ascension_rate());
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
}

TEST_CASE("SkyWatcher Telescope Driver - Device metadata", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(3);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Sky-Watcher Motor Controller Mount Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore SkyWatcher Driver v0.1");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "SkyWatcher_3");
    CHECK(driver->get_alignment_mode() == AlignmentMode::GermanPolar);
    CHECK(driver->get_equatorial_system() == EquatorialSystem::Topocentric);
}

TEST_CASE("SkyWatcher Telescope Driver - Not connected throws", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    require_alpaca_error([&] { driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_slewing(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_is_pulse_guiding(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->slew_to_coordinates(12.0, 45.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(12.0, 45.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->sync_to_coordinates(12.0, 45.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move_axis(0, 1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->pulse_guide(0, 100); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->park(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->unpark(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->get_utc_date(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("SkyWatcher Telescope Driver - Unsupported actions", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("SkyWatcher Telescope Driver - Target persistence", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    REQUIRE_THROWS(driver->get_target_right_ascension());
    REQUIRE_THROWS(driver->get_target_declination());
    require_alpaca_error([&] { driver->get_target_right_ascension(); },
                         alpacacore::AlpacaError::ValueNotSet);

    driver->set_target_right_ascension(12.5);
    driver->set_target_declination(-30.25);
    CHECK(driver->get_target_right_ascension() == 12.5);
    CHECK(driver->get_target_declination() == -30.25);

    // Independence: updating one leaves the other intact.
    driver->set_target_right_ascension(3.0);
    CHECK(driver->get_target_declination() == -30.25);

    // Site properties validate their documented ranges (disconnected OK).
    driver->set_site_latitude(51.5);
    CHECK(driver->get_site_latitude() == 51.5);
    driver->set_site_longitude(-0.12);
    CHECK(driver->get_site_longitude() == -0.12);
    driver->set_site_elevation(35.0);
    CHECK(driver->get_site_elevation() == 35.0);

    CHECK(driver->get_slew_settle_time() == 0);
    driver->set_slew_settle_time(2);
    CHECK(driver->get_slew_settle_time() == 2);

    CHECK_FALSE(driver->get_tracking_rates().empty());
    CHECK(driver->get_tracking_rate() == 0);
}

TEST_CASE("SkyWatcher Telescope Driver - Value range validation", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    require_alpaca_error([&] { driver->set_target_right_ascension(-0.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_right_ascension(24.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_declination(-90.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_declination(90.1); },
                         alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&] { driver->set_site_latitude(-90.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_site_latitude(90.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_site_longitude(-180.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_site_longitude(180.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_site_elevation(-300.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_site_elevation(10000.1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_slew_settle_time(-1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_aperture_diameter(-1.0); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_focal_length(-1.0); },
                         alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("SkyWatcher Telescope Driver - State machine", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    // Disconnected state machine facts that need no hardware.
    CHECK_FALSE(driver->get_at_park());
    CHECK_FALSE(driver->get_at_home());
    CHECK(driver->get_declination_rate() == 0.0);
    CHECK(driver->get_right_ascension_rate() == 0.0);

    // Sidereal time is computed host-side and must be a valid hour value even
    // when disconnected.
    double lst = driver->get_sidereal_time();
    CHECK(lst >= 0.0);
    CHECK(lst < 24.0);

    // Guide rate defaults to a sane sub-sidereal value.
    auto guide_rate = driver->get_guide_rate();
    CHECK(guide_rate.ra > 0.0);
    CHECK(guide_rate.dec > 0.0);
    CHECK(guide_rate.ra <= 360.0 / 86164.0905);
}

TEST_CASE("SkyWatcher Telescope Driver - Axis rate ranges", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    auto primary = driver->get_axis_rate_range(0);
    REQUIRE(primary.first == 0.0);
    REQUIRE(primary.second > primary.first);

    auto secondary = driver->get_axis_rate_range(1);
    REQUIRE(secondary.first == 0.0);
    REQUIRE(secondary.second > secondary.first);

    // Tertiary axis: empty range set, and the single-range getter throws
    // InvalidValue (not MethodNotImplemented) per ASCOM semantics.
    auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());
    require_alpaca_error([&] { driver->get_axis_rate_range(2); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->get_axis_rate_ranges(5); },
                         alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("SkyWatcher Telescope Driver - Unsupported methods", "[skywatcher][telescope][unit]") {
    auto driver = make_driver(0);

    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->slew_to_alt_az(45.0, 180.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->slew_to_alt_az_async(45.0, 180.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->sync_to_alt_az(45.0, 180.0); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->set_side_of_pier(0); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_declination_rate(1.0); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_right_ascension_rate(1.0); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_tracking_rate(1); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
}
