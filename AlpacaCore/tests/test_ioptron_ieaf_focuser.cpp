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
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/version.h>

#include <functional>

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

} // namespace

TEST_CASE("iOptron iEAF Focuser Driver - Defaults", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "iOptron iEAF");
    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_temp_comp_available() == false);
}

TEST_CASE("iOptron iEAF Focuser Driver - Metadata", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iEAF Electronic Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iEAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "IOPTRON_IEAF_3");
}

TEST_CASE("iOptron iEAF Focuser Driver - Not connected throws", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(1, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(1000); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("iOptron iEAF Focuser Driver - Unsupported actions", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("iOptron iEAF Focuser Driver - Static capabilities", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    // Range limits are fixed by the protocol (7-digit move field, hardware
    // range 0..99999) and do not require a live connection.
    CHECK(driver->get_max_step() == 99999);
    CHECK(driver->get_max_increment() == 99999);

    // Step size in microns is not exposed by the iEAF protocol.
    require_alpaca_error([&]() { driver->get_step_size(); },
                         alpacacore::AlpacaError::PropertyNotImplemented);
}

TEST_CASE("iOptron iEAF Focuser Driver - Unsupported methods", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    // No temperature compensation in hardware: available=false, get=false,
    // and a write must report NotImplemented (not a generic driver error),
    // regardless of connection state.
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
    require_alpaca_error([&]() { driver->set_temp_comp(true); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_temp_comp(false); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("iOptron iEAF Focuser Driver - State machine", "[ioptron][focuser][unit]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    // Platform 7 DeviceState while disconnected: operational getters throw and
    // are omitted, leaving just the TimeStamp; no non-compliant "Connected".
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("iOptron iEAF Focuser Driver - Unique IDs", "[ioptron][focuser][unit]") {
    auto driver0 = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::ioptron::create_ieaf_focuser(1, "/dev/ttyUSB1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "IOPTRON_IEAF_0");
    CHECK(driver1->get_unique_id() == "IOPTRON_IEAF_1");
}
