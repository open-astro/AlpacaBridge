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
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_zwo_efw_sdk.h"

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

TEST_CASE("ZWO EFW Filter Wheel Driver - Defaults", "[zwo][filterwheel][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO EFW");
}

TEST_CASE("ZWO EFW Filter Wheel Driver - Disconnected Behavior", "[zwo][filterwheel][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
    // Range validation precedes the connection check (ASCOM precedence): a negative
    // position is InvalidValue even while disconnected (matches the ToupTek AFW).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0}));
    REQUIRE_NOTHROW(driver->set_names({"L"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L"});

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO EFW Filter Wheel Driver - Device metadata", "[zwo][filterwheel][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO EFW Filter Wheel Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO EFW Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_EFW_3");
}

TEST_CASE("ZWO EFW Filter Wheel Driver - Device Number Assignment", "[zwo][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(1, 0);
    auto d5 = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(5, 0);

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d5->get_device_number() == 5);
}

TEST_CASE("ZWO EFW Filter Wheel Driver - Unique IDs", "[zwo][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(1, 0);

    CHECK(d0->get_unique_id() != d1->get_unique_id());
}

// Issue #1500: EFW_ERROR_REMOVED reaches the driver as NotConnected. Positive evidence the transport is gone
// (decision 0009 point 2a), so Connected reads false at once.
// Falsified by: the Position refill counting a NotConnected SDK error as one more failed poll.
TEST_CASE("ZWO EFW Filter Wheel Driver - A wheel removed from USB reads Connected false",
          "[zwo][filterwheel][unit][fake-sdk][link]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 0);

    sdk.remove();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_THROWS(driver->get_position());
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
}

// Issue #1500: the replug gives the EFW a new SDK id; one Connect finds it again.
// Falsified by: connect opening the id the driver was created with.
TEST_CASE("ZWO EFW Filter Wheel Driver - A replugged wheel is reopened by one Connect",
          "[zwo][filterwheel][unit][fake-sdk][link]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.remove();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_THROWS(driver->get_position());
    sdk.replug(4);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(driver->get_position() == 0);
}

// Issue #1500, the rig step: the client toggles Connected off and on across the replug. The Disconnect must
// not throw for the dead handle, and the Connect must find the wheel under its new id (decision 0009 point 4).
// Falsified by: Disconnect rethrowing the NotConnected close of a removed wheel, or Connect reopening the id
// the driver was created with.
TEST_CASE("ZWO EFW Filter Wheel Driver - A Connected toggle across a replug reopens the wheel",
          "[zwo][filterwheel][unit][fake-sdk][link]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);

    sdk.remove();
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
    sdk.replug(4);

    CHECK_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK(sdk.last_open_id == 4);
}

// Issue #1500: every Connect binds the serial the wheel had, so a replug while the client is disconnected that
// swaps two wheels' SDK ids still opens the same wheel.
// Falsified by: Connect opening the id the driver was created with, or the first enumerated wheel.
TEST_CASE("ZWO EFW Filter Wheel Driver - Two wheels swapped while disconnected are told apart by serial",
          "[zwo][filterwheel][unit][fake-sdk][link]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    sdk.second_id = 6;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->set_connected(false);

    sdk.remove();
    sdk.second_id = 5;
    sdk.replug(6);

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(sdk.last_open_id == 6);
}

// Issue #1500: Disconnect from the lost state clears the driver without throwing (invariant 5).
// Falsified by: Disconnect rethrowing the NotConnected close of a removed wheel.
TEST_CASE("ZWO EFW Filter Wheel Driver - Disconnect after a removal does not throw",
          "[zwo][filterwheel][unit][fake-sdk][link]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.remove();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_THROWS(driver->get_position());

    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}
