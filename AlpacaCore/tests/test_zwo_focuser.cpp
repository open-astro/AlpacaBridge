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
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
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

// Scripted EAF SDK: one focuser (id 3), single-threaded tests only.
class FakeEAFSDK final : public alpacacore::vendor::zwo::ZWOEAFSDK {
public:
    using Info = alpacacore::vendor::zwo::ZWOEAFFocuserInfo;

    int id{3};
    int max_step{60000};
    int step_range{1000};
    int position{500};
    bool moving{false};
    double temperature{12.5};
    bool fail_reads{false};
    bool fail_temperature{false};
    bool temperature_not_supported{false};
    int moving_calls{0};
    int position_calls{0};
    int temperature_calls{0};
    int max_step_calls{0};
    int step_range_calls{0};

    std::vector<Info> enumerate_focusers() override { return {Info{id, "EAF", max_step}}; }
    bool get_focuser_info_by_id(int focuser_id, Info& info) override {
        if (focuser_id != id) {
            return false;
        }
        info = Info{id, "EAF", max_step};
        return true;
    }
    bool get_focuser_info_by_index(int, Info& info) override { return get_focuser_info_by_id(id, info); }
    void open_focuser(int) override {}
    void close_focuser(int) override {}
    bool is_moving(int) override {
        ++moving_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return moving;
    }
    int get_position(int) override {
        ++position_calls;
        if (fail_reads) {
            throw std::runtime_error("link down");
        }
        return position;
    }
    void move(int, int target) override { position = target; }
    void stop(int) override { moving = false; }
    int get_max_step(int) override {
        ++max_step_calls;
        return max_step;
    }
    int get_step_range(int) override {
        ++step_range_calls;
        return step_range;
    }
    double get_temperature(int) override {
        ++temperature_calls;
        if (temperature_not_supported) {
            throw alpacacore::AlpacaException("EAF temperature not supported", alpacacore::AlpacaError::NotImplemented);
        }
        if (fail_temperature) {
            throw std::runtime_error("temp sensor down");
        }
        return temperature;
    }
    std::string get_serial_number(int) override { return "EAF-SN-1"; }
    std::string get_firmware_version(int) override { return "1.0"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }
};

void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

TEST_CASE("ZWO EAF Focuser Driver - Defaults", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO EAF");
}

TEST_CASE("ZWO EAF Focuser Driver - Disconnected Behavior", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    // open-astro#309: TempCompAvailable and TempComp are properties, so a
    // disconnected read refuses rather than answering. These used to assert
    // the value, which is what let the missing connection check survive.
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, and TimeStamp itself is withheld too, leaving the
    // ASCOM-required empty list; the old non-compliant "Connected"
    // entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE_FALSE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    // open-astro#309: the connection check precedes the not-implemented
    // answer, so a disconnected write refuses on connection grounds.
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO EAF Focuser Driver - Device metadata", "[zwo][focuser][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO EAF Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ZWO_EAF_3");
}

TEST_CASE("ZWO EAF Focuser Driver - Device Number Assignment", "[zwo][focuser][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);
    auto driver5 = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
}

TEST_CASE("ZWO EAF Focuser Driver - Unique IDs", "[zwo][focuser][unit]") {
    auto driver_a = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(0, 0);
    auto driver_b = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(1, 0);

    REQUIRE(!driver_a->get_unique_id().empty());
    REQUIRE(!driver_b->get_unique_id().empty());
    CHECK(driver_a->get_unique_id() != driver_b->get_unique_id());
}

// open-astro#294: DeviceState used to round-trip the device once per property.
// Falsified by: zwo_focuser_driver.cpp read_status() calling the SDK on every
// read instead of through status_cache_.get() (position_calls 2, not 1).
TEST_CASE("ZWO EAF Focuser Driver - DeviceState is one device read, served from the TTL cache",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_device_state().size() == 4);  // IsMoving, Position, Temperature, TimeStamp
    CHECK(sdk.moving_calls == 1);
    CHECK(sdk.position_calls == 1);
    CHECK(sdk.temperature_calls == 1);

    (void)driver->get_device_state();
    CHECK(driver->get_position() == 500);
    CHECK(sdk.position_calls == 1);  // inside the TTL: no device I/O

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(sdk.position_calls == 2);
}

// Falsified by: zwo_focuser_driver.cpp get_max_step() calling sdk_.get_max_step()
// instead of returning max_step_ (max_step_calls 3 after the reads, not 1).
TEST_CASE("ZWO EAF Focuser Driver - Static members are read at connect and reset on disconnect",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(sdk.max_step_calls == 1);
    CHECK(sdk.step_range_calls == 1);

    for (int i = 0; i < 3; ++i) {
        CHECK(driver->get_max_step() == 60000);
        CHECK(driver->get_max_increment() == 1000);
        CHECK(driver->get_name() == "EAF");
    }
    CHECK(sdk.max_step_calls == 1);
    CHECK(sdk.step_range_calls == 1);

    driver->set_connected(false);
    sdk.max_step = 30000;
    driver->set_connected(true);
    CHECK(driver->get_max_step() == 30000);
    CHECK(driver->get_max_increment() == 1000);
    CHECK(sdk.max_step_calls == 2);

    driver->move(100);  // range check uses the cached MaxStep
    require_alpaca_error([&]() { driver->move(30001); }, alpacacore::AlpacaError::InvalidValue);
}

// Falsified by: zwo_focuser_driver.cpp move() dropping its
// status_cache_.invalidate() (Position keeps the pre-move 500 for a TTL).
TEST_CASE("ZWO EAF Focuser Driver - A move invalidates the cached frame", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    driver->move(900);
    CHECK(driver->get_position() == 900);
}

// Falsified by: ttl_status_cache.h get() dropping its health_.faulted() throw
// (the third failure reports the raw SDK error, not "communications compromised").
TEST_CASE("ZWO EAF Focuser Driver - A dead link refuses reads instead of serving the cache",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 500);

    sdk.fail_reads = true;
    wait_past_status_ttl();
    for (int i = 0; i < 2; ++i) {
        CHECK_THROWS(driver->get_position());
    }
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_device_state().size() == 1);  // TimeStamp only

    sdk.fail_reads = false;
    CHECK(driver->get_position() == 500);
}

// Falsified by: zwo_focuser_driver.cpp halt() dropping its
// status_cache_.invalidate() (IsMoving keeps answering true for a TTL).
TEST_CASE("ZWO EAF Focuser Driver - Halt drops the cached frame", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.moving = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_is_moving() == true);
    driver->halt();
    CHECK(driver->get_is_moving() == false);
}

// Falsified by: zwo_focuser_driver.cpp read_status() letting the temperature
// failure escape the refill (Position stops answering) or dropping the SDK text.
TEST_CASE("ZWO EAF Focuser Driver - A temperature failure leaves Position answering",
          "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.fail_temperature = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    try {
        (void)driver->get_temperature();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("temp sensor down") != std::string::npos);
    }
}

TEST_CASE("ZWO EAF Focuser Driver - A temperature error keeps its mapped code", "[zwo][focuser][unit][fake-sdk]") {
    FakeEAFSDK sdk;
    sdk.temperature_not_supported = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 500);
    try {
        (void)driver->get_temperature();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotImplemented);
    }
}
