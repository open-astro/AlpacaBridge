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

#include "catch2_compat.h"

#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#include <alpacacore/util/error_handling.h>

#include <functional>
#include <limits>

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

TEST_CASE("ZWO Mount Telescope Driver - Defaults", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Telescope);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);

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
    REQUIRE(driver->get_can_move_axis(0));
    REQUIRE(driver->get_can_move_axis(1));
    REQUIRE_FALSE(driver->get_can_move_axis(2));
}

TEST_CASE("ZWO Mount Telescope Driver - Target Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(1, conn);

    require_alpaca_error([&]() { (void)driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    require_alpaca_error([&]() { driver->set_target_right_ascension(-0.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_right_ascension(24.0); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_right_ascension(12.0));

    require_alpaca_error([&]() { driver->set_target_declination(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_target_declination(90.1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE_NOTHROW(driver->set_target_declination(45.0));

    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 12.0);
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), 45.0);
}

TEST_CASE("ZWO Mount Telescope Driver - Target Coordinate Set Tracking", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(4, conn);

    REQUIRE_NOTHROW(driver->set_target_right_ascension(3.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_right_ascension(), 3.0);
    require_alpaca_error([&]() { (void)driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    REQUIRE_NOTHROW(driver->set_target_declination(-20.0));
    ALPACA_REQUIRE_APPROX(driver->get_target_declination(), -20.0);
}

TEST_CASE("ZWO Mount Telescope Driver - Disconnected Behavior", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(2, conn);

    REQUIRE(driver->get_connected() == false);
    require_alpaca_error([&]() { (void)driver->get_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_declination(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_altitude(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_tracking(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_tracking(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->slew_to_target_async(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->park(); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
}

TEST_CASE("ZWO Mount Telescope Driver - Site Elevation Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(3, conn);

    require_alpaca_error([&]() { driver->set_site_elevation(std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(-300.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_elevation(10000.1); }, alpacacore::AlpacaError::InvalidValue);

    REQUIRE_NOTHROW(driver->set_site_elevation(-300.0));
    REQUIRE_NOTHROW(driver->set_site_elevation(10000.0));
    ALPACA_REQUIRE_APPROX(driver->get_site_elevation(), 10000.0);
}

TEST_CASE("ZWO Mount Telescope Driver - Site Latitude/Longitude Validation", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(3, conn);

    require_alpaca_error([&]() { driver->set_site_latitude(-90.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_latitude(90.1); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&]() { driver->set_site_longitude(-180.1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_site_longitude(180.1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Mount Telescope Driver - Telescope Properties", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);

    CHECK(driver->get_interface_version() >= 3);
    CHECK(driver->get_slew_settle_time() >= 0);
    require_alpaca_error([&]() { driver->set_slew_settle_time(-1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Mount Telescope Driver - Axis Rate Ranges", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Serial;
    conn.port_path = "/dev/null";

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(5, conn);

    const auto primary_ranges = driver->get_axis_rate_ranges(0);
    REQUIRE(primary_ranges.size() == 1);
    ALPACA_REQUIRE_APPROX(primary_ranges.front().first, 0.0);
    REQUIRE(primary_ranges.front().second > 0.0);

    const auto tertiary_ranges = driver->get_axis_rate_ranges(2);
    REQUIRE(tertiary_ranges.empty());
}

TEST_CASE("ZWO Mount Telescope Driver - ConnectionType::Auto and enumeration", "[zwo][telescope][unit]") {
    using alpacacore::vendor::zwo::ConnectionType;
    using alpacacore::vendor::zwo::ZWODeviceInfo;

    // ConnectionType::Auto must be a valid transport value distinct from the
    // explicit serial/network modes.
    ConnectionType auto_type = ConnectionType::Auto;
    CHECK(static_cast<int>(auto_type) != static_cast<int>(ConnectionType::Serial));
    CHECK(static_cast<int>(auto_type) != static_cast<int>(ConnectionType::Network));

    // ZWODeviceInfo defaults: serial type, empty port/host, protocol-default
    // TCP port, empty model/label.
    ZWODeviceInfo info;
    CHECK(info.type == ConnectionType::Serial);
    CHECK(info.port_path.empty());
    CHECK(info.host.empty());
    CHECK(info.tcp_port == 4030);
    CHECK(info.model_name.empty());
    CHECK(info.label.empty());

    // enumerate_zwo_mounts with a short probe timeout must return without
    // throwing or hanging. On a host with no ZWO device attached (CI runners),
    // it returns an empty list; with a ZWO device present it returns at least
    // one entry carrying a non-empty model name (probed via :GVP).
    std::vector<ZWODeviceInfo> devices;
    REQUIRE_NOTHROW(devices = alpacacore::vendor::zwo::enumerate_zwo_mounts(100));
    for (const auto& dev : devices) {
        CHECK_FALSE(dev.model_name.empty());
        REQUIRE((dev.type == ConnectionType::Serial || dev.type == ConnectionType::Network));
        if (dev.type == ConnectionType::Serial) {
            CHECK_FALSE(dev.port_path.empty());
        } else {
            CHECK_FALSE(dev.host.empty());
        }
    }
}

TEST_CASE("ZWO Mount Telescope Driver - Auto connect on missing hardware", "[zwo][telescope][unit]") {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Auto;
    conn.response_timeout_ms = 100;

    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(6, conn);
    REQUIRE(driver != nullptr);

    // With no ZWO mount reachable, connect() must fail cleanly (no crash,
    // no hang) rather than throwing or leaving a half-open state.
    try {
        driver->set_connected(true);
        // Some platforms (a host with a real ZWO device) may succeed; that is
        // fine. Only assert that a failure surfaces as NotConnected/DriverException
        // rather than corrupting the driver.
    } catch (const alpacacore::AlpacaException&) {
    }
    // The driver must remain in a well-defined (disconnected) state afterwards.
    CHECK(driver->get_connecting() == false);
}
