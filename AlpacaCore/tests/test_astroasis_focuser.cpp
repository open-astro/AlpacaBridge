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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <variant>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("Astroasis Focuser Driver - Defaults", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Astroasis Oasis Focuser");
}

TEST_CASE("Astroasis Focuser Driver - Metadata", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    CHECK(driver->get_description() == "Astroasis Oasis Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Astroasis Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ASTROASIS_FOCUSER_0");
}

TEST_CASE("Astroasis Focuser Driver - Disconnected Behavior", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    REQUIRE(driver->get_temp_comp_available() == false);
    REQUIRE(driver->get_temp_comp() == false);
    REQUIRE(driver->get_supported_actions().empty());

    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Astroasis Focuser Driver - Connecting State", "[astroasis][focuser][unit]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Astroasis Focuser Driver - Device Number Assignment", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");
    auto driver5 = alpacacore::vendor::astroasis::create_astroasis_focuser(5, "/dev/hidraw2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Astroasis Focuser Driver - Unique IDs", "[astroasis][focuser][unit]") {
    auto driver0 = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");
    auto driver1 = alpacacore::vendor::astroasis::create_astroasis_focuser(1, "/dev/hidraw1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "ASTROASIS_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "ASTROASIS_FOCUSER_1");
}
