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
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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

TEST_CASE("ZWO Dew Heater Switch Driver - Defaults", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO Dew Heater");
    REQUIRE(driver->get_max_switch() == 1);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Disconnected Behavior", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE_FALSE(driver->get_connected());
    // Reading or writing switch value requires connection (dew caps loaded from camera)
    REQUIRE_THROWS(driver->get_switch_value(0));
    REQUIRE_THROWS(driver->set_switch_value(0, 0.0));
    REQUIRE_THROWS(driver->get_switch(0));
    REQUIRE_THROWS(driver->set_switch(0, false));
}

TEST_CASE("ZWO Dew Heater Switch Driver - Invalid Switch ID", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver->get_max_switch() == 1);
    REQUIRE_THROWS(driver->get_can_write(1));
    REQUIRE_THROWS(driver->get_switch_name(1));

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(1); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Device metadata", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(3, 0);

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO camera dew heater switch");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO Dew Heater Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_DEW_3");
}

TEST_CASE("ZWO Dew Heater Switch Driver - Unsupported actions", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    REQUIRE(driver != nullptr);

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO Dew Heater Switch Driver - ASCOM Error Codes", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, false); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO Dew Heater Switch Driver - Disconnected DeviceState", "[zwo][switch][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0);

    {
        // DeviceState is the empty list while disconnected: the SwitchDriver
        // base builds it from the public getters, which throw NotConnected and
        // are omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
        const auto state = driver->get_device_state();
        REQUIRE(state.empty());
    }
}

// ---- open-astro#294: one status read per DeviceState, static members free ----

namespace {

struct FakeDewSdk final : alpacacore::vendor::zwo::ZWODewHeaterSdk {
    std::atomic<int> reads{0};
    std::atomic<bool> fail_reads{false};
    long heater{40};

    std::vector<alpacacore::vendor::zwo::ZWOCameraInfo> enumerate_cameras() override { return {}; }
    bool get_camera_info_by_id(int, alpacacore::vendor::zwo::ZWOCameraInfo&) override { return false; }
    void open_camera(int) override {}
    void init_camera(int) override {}
    void close_camera(int) override {}
    std::vector<alpacacore::vendor::zwo::ZWOControlCaps> get_control_caps(int) override {
        alpacacore::vendor::zwo::ZWOControlCaps cap;
        cap.type = alpacacore::vendor::zwo::ZWOControlType::AntiDewHeater;
        cap.max_value = 100;
        cap.is_writable = true;
        return {cap};
    }
    bool get_control_value(int, alpacacore::vendor::zwo::ZWOControlType, long& value, bool& is_auto) override {
        ++reads;
        if (fail_reads) {
            return false;
        }
        value = heater;
        is_auto = false;
        return true;
    }
    void set_control_value(int, alpacacore::vendor::zwo::ZWOControlType, long value, bool) override { heater = value; }
    std::string get_serial_number(int) override { return "FAKE1"; }
};

void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

}  // namespace

TEST_CASE("ZWO Dew Heater Switch Driver - DeviceState reads the camera once per status frame", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_switch_value(0) == 40.0);
    CHECK(driver->get_switch(0));
    // Before the cache each of these was its own SDK read.
    CHECK(sdk.reads == 1);

    const auto state = driver->get_device_state();
    CHECK_FALSE(state.empty());
    CHECK(sdk.reads == 1);

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(sdk.reads == 2);
}

TEST_CASE("ZWO Dew Heater Switch Driver - static members cost no camera reads", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);

    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_max_switch() == 1);
        CHECK(driver->get_switch_name(0) == "DewHeater");
        CHECK_FALSE(driver->get_switch_description(0).empty());
        CHECK(driver->get_min_switch_value(0) == 0.0);
        CHECK(driver->get_max_switch_value(0) == 100.0);
        CHECK(driver->get_switch_step(0) == 1.0);
        CHECK(driver->get_can_write(0));
    }
    CHECK(sdk.reads == 0);
}

TEST_CASE("ZWO Dew Heater Switch Driver - a write and a reconnect drop the cached frame", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 40.0);

    driver->set_switch_value(0, 10.0);
    CHECK(driver->get_switch_value(0) == 10.0);

    sdk.heater = 55;
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 55.0);
}

TEST_CASE("ZWO Dew Heater Switch Driver - a dead link is not served from the cache", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 40.0);

    sdk.fail_reads = true;
    wait_past_status_ttl();
    for (int i = 0; i < 3; ++i) {
        CHECK_THROWS_AS(driver->get_switch_value(0), alpacacore::AlpacaException);
    }
    try {
        (void)driver->get_switch_value(0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }
    CHECK(driver->get_connected());

    sdk.fail_reads = false;
    CHECK(driver->get_switch_value(0) == 40.0);
}
