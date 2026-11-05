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

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_zwo_efw_sdk.h"
#include "getter_stall_probe.h"

namespace {

using alpacacore::test::require_alpaca_error;

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

// Falsified by: get_device_firmware() in the EFW driver returning the cache without the connected check (the
// pre-connect and post-disconnect nullopt), dropping the clear at disconnect, or calling the SDK per call (the
// firmware_calls count).
TEST_CASE("ZWO EFW Filter Wheel Driver - Firmware is read once at connect and cleared at disconnect",
          "[zwo][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    sdk.firmware = "2.3.4";
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    CHECK_FALSE(driver->get_device_firmware().has_value());
    driver->set_connected(true);
    CHECK(sdk.firmware_calls == 1);
    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_device_firmware() == std::optional<std::string>("2.3.4"));
    }
    CHECK(sdk.firmware_calls == 1);  // the hook made no SDK call
    driver->set_connected(false);
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

// Falsified by: the EFW driver's connect letting a failed firmware read propagate (the connect then throws), or
// reporting an empty string as a firmware.
TEST_CASE("ZWO EFW Filter Wheel Driver - A failed or empty firmware read does not fail the connect",
          "[zwo][filterwheel][unit][fake-sdk]") {
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    sdk.fail_firmware = true;
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    driver->set_connected(false);

    sdk.fail_firmware = false;
    sdk.firmware.clear();
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    driver->set_connected(false);
}

// Decision 0016. Falsified by: get_device_firmware() in the EFW driver taking io_mutex_ or mutex_-then-I/O (the
// hook then waits the whole 300 ms status read).
TEST_CASE("ZWO EFW Filter Wheel Driver - Firmware answers while a status read is in flight",
          "[zwo][filterwheel][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    alpacacore::test::FakeZWOEFWSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.position_delay = kHoldOpen;
    std::thread reader([&] {
        try {
            (void)driver->get_position();
        } catch (const std::exception&) {
        }
    });
    for (int i = 0; i < 200 && sdk.position_entered.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(sdk.position_entered.load() > 0);
    CHECK(timed([&] { CHECK(driver->get_device_firmware().has_value()); }) < kGetterBudget);
    reader.join();
    driver->set_connected(false);
}
