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
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../src/vendors/zwo/zwo_fields.h"
#include "catch2_compat.h"
#include "fake_zwo_sdk.h"
#include "getter_stall_probe.h"

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

using FakeDewSdk = alpacacore::test::FakeDewHeaterSdk;

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

// Falsified by: zwo_switch_driver.cpp resolve_camera_id, index into the list (cameras[index]) instead of
// matching slot_index
TEST_CASE("ZWO Dew Heater Switch Driver - an index binds the SDK slot, not the list position", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    // Slot 0 was unreadable and skipped: the one readable camera sits at SDK slot 1.
    alpacacore::vendor::zwo::ZWOCameraInfo cam;
    cam.camera_id = 7;
    cam.name = "ASI120MM Mini";
    cam.slot_index = 1;
    sdk.cameras = {cam};

    auto bound = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 1, sdk);
    bound->set_connected(true);
    CHECK(bound->get_connected());

    auto skipped = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0, sdk);
    require_alpaca_error([&]() { skipped->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(skipped->get_connected());
}

// ---- #1284: Auto, serial and capability refusal on the dew heater's own seam ----

namespace {

alpacacore::vendor::zwo::ZWOCameraInfo dew_camera(int id, int slot, const std::string& name) {
    alpacacore::vendor::zwo::ZWOCameraInfo cam;
    cam.camera_id = id;
    cam.slot_index = slot;
    cam.name = name;
    return cam;
}

// The rig of the bug: an ASI290MM Mini (no heater) at slot 0 beside an ASI2600MM Pro at slot 1.
void add_mini_and_pro(FakeDewSdk& sdk, bool mini_has_heater) {
    sdk.cameras = {dew_camera(7, 0, "ZWO ASI290MM Mini"), dew_camera(9, 1, "ZWO ASI2600MM Pro")};
    sdk.heater_by_id = {{7, mini_has_heater}, {9, true}};
    sdk.serial_by_id = {{7, "2d0bd20224000900"}, {9, "2e18870f28000900"}};
}

}  // namespace

// Falsified by: zwo_switch_driver.cpp resolve_camera_id, call list_dew_heater_cameras before the
// camera_bound_by_id_only() return
TEST_CASE("ZWO Dew Heater Switch Driver - an id-only binding connects without touching other cameras",
          "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    add_mini_and_pro(sdk, true);
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 9, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_value(0) == 40.0);
    driver->set_connected(false);

    CHECK(sdk.enumerations == 0);
    const auto touched = sdk.touched_ids();
    CHECK(touched.count(7) == 0);
    CHECK(touched.count(9) == 1);
}

// Falsified by: zwo_switch_driver.cpp find_dew_caps, skip the lazy listing for an id-only binding
TEST_CASE("ZWO Dew Heater Switch Driver - an id-only camera with no heater names the one that has it",
          "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    add_mini_and_pro(sdk, false);
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 7, sdk);
    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == static_cast<int>(alpacacore::AlpacaError::NotImplemented));
        CHECK(std::string(ex.what()).find("ZWO ASI2600MM Pro") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("ZWO Dew Heater Switch Driver - Auto binds the one camera with a heater", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    add_mini_and_pro(sdk, false);
    auto driver =
        alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, alpacacore::util::kAutoThermalCamera, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_value(0) == 40.0);
    CHECK(sdk.last_read_id == 9);
    driver->set_connected(false);
}

TEST_CASE("ZWO Dew Heater Switch Driver - a serial binds its camera over a conflicting index", "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    add_mini_and_pro(sdk, true);
    // Saved at index 0, but the serial is the Pro's: after a USB reorder the serial wins.
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0, sdk, "2e18870f28000900");
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_value(0) == 40.0);
    CHECK(sdk.last_read_id == 9);
    driver->set_connected(false);
}

TEST_CASE("ZWO Dew Heater Switch Driver - a pinned camera with no heater names the one that has it",
          "[zwo][switch][unit]") {
    FakeDewSdk sdk;
    add_mini_and_pro(sdk, false);
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(0, 0, sdk);
    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == static_cast<int>(alpacacore::AlpacaError::NotImplemented));
        const std::string message = ex.what();
        CHECK(message.find("ZWO ASI290MM Mini") != std::string::npos);
        CHECK(message.find("ZWO ASI2600MM Pro") != std::string::npos);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("ZWO dew heater binding - a serial wins over cameraId and a bare cameraId binds the id",
          "[zwo][switch][catalog][unit]") {
    using alpacacore::catalog::DeviceConfig;
    DeviceConfig id_only;
    id_only.set("cameraId", std::int64_t{4});
    const auto by_id = alpacacore::catalog::zwo_dew_heater_binding(id_only);
    REQUIRE(by_id.camera_id.has_value());
    CHECK(*by_id.camera_id == 4);

    DeviceConfig id_and_serial = id_only;
    id_and_serial.set("cameraIndex", std::int64_t{1});
    id_and_serial.set("serialNumber", std::string{"2e18870f28000900"});
    const auto by_serial = alpacacore::catalog::zwo_dew_heater_binding(id_and_serial);
    CHECK_FALSE(by_serial.camera_id.has_value());
    CHECK(by_serial.camera_index == 1);
    CHECK(by_serial.serial == "2e18870f28000900");

    const auto automatic = alpacacore::catalog::zwo_dew_heater_binding(DeviceConfig{});
    CHECK_FALSE(automatic.camera_id.has_value());
    CHECK(automatic.camera_index == alpacacore::util::kAutoThermalCamera);
    CHECK(automatic.serial.empty());
}

// Decision 0016: UniqueID needs no device access, so a connect that holds the SDK open must not park it.
// (GetSwitchName throws NotConnected until the connect publishes, so it is timed once connected below.)
// Falsified by: zwo_switch_driver.cpp set_connected(true) holding mutex_ across open_camera() (get_unique_id() then
// waits the whole 300 ms open).
TEST_CASE("ZWO Dew Heater Switch Driver - Name and UniqueID answer while a connect holds the SDK open",
          "[zwo][switch][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeDewSdk sdk;
    sdk.open_delay_ms = static_cast<int>(kHoldOpen.count());
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);

    driver->connect();
    REQUIRE(wait_for_flag(sdk.open_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(driver->get_connecting());  // the open is still in flight, so the timings above are not vacuous

    REQUIRE(wait_until_connected(*driver));
    CHECK(driver->get_unique_id() == "ZWO_DEW_SN_FAKE1");
    driver->set_connected(false);
}

// Falsified by: zwo_switch_driver.cpp get_switch_name() or get_unique_id() taking io_mutex_ (the cached read then
// waits the 300 ms status refill).
TEST_CASE("ZWO Dew Heater Switch Driver - Identity and names answer while a status read is in flight",
          "[zwo][switch][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);
    sdk.read_delay_ms = static_cast<int>(kHoldOpen.count());

    std::thread reader([&] {
        try {
            (void)driver->get_switch_value(0);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.read_entered));

    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(time_call([&] { CHECK(driver->get_switch_name(0) == "DewHeater"); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_switch_description(0); }) < kGetterBudget);
    CHECK(time_call([&] { CHECK(driver->get_max_switch_value(0) == 100.0); }) < kGetterBudget);
    CHECK(sdk.busy.load() > 0);  // the refill is still in flight

    reader.join();
    driver->set_connected(false);
}

// A disconnect must wait for the exchange in flight, never close the camera under it (the I/O lock).
// Falsified by: zwo_switch_driver.cpp set_connected(false) calling close_camera() without io_mutex_, or
// set_switch_value() reading the id and then calling the SDK without it (closed_while_busy is set).
TEST_CASE("ZWO Dew Heater Switch Driver - A disconnect waits for the exchange in flight instead of closing under it",
          "[zwo][switch][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);

    std::thread worker;
    SECTION("a status read") {
        sdk.read_delay_ms = static_cast<int>(kHoldOpen.count());
        worker = std::thread([&] {
            try {
                (void)driver->get_switch_value(0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.read_entered));
    }
    SECTION("a write") {
        sdk.write_delay_ms = static_cast<int>(kHoldOpen.count());
        worker = std::thread([&] {
            try {
                driver->set_switch_value(0, 20.0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.write_entered));
    }

    CHECK(time_call([&] { driver->set_connected(false); }) < std::chrono::seconds(5));
    worker.join();
    CHECK_FALSE(sdk.closed_while_busy.load());
    CHECK_FALSE(driver->get_connected());
}

// A disconnect that wins the I/O lock while status reads queue behind a write is not a link failure: every reader
// gets NotConnected, none the "communications compromised" fault that three counted failures would latch.
// Falsified by: zwo_switch_driver.cpp read_status() throwing NotConnected inside the cache refill instead of returning
// the closed frame (the third queued reader then gets DriverException "communications compromised").
TEST_CASE("ZWO Dew Heater Switch Driver - A disconnect during queued status reads is not a link failure",
          "[zwo][switch][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);
    driver->set_connected(true);

    sdk.write_delay_ms = static_cast<int>(kHoldOpen.count());
    std::thread mover([&] {
        try {
            driver->set_switch_value(0, 20.0);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.write_entered));

    constexpr int kReaders = 8;
    std::atomic<int> not_connected{0};
    std::atomic<int> other{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            try {
                (void)driver->get_switch_value(0);
                ++other;
            } catch (const alpacacore::AlpacaException& e) {
                (e.error_code() == alpacacore::AlpacaError::NotConnected ? not_connected : other)++;
            } catch (const std::exception&) {
                ++other;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // every reader is parked on the write
    driver->set_connected(false);
    mover.join();
    for (auto& reader : readers) {
        reader.join();
    }

    CHECK(other.load() == 0);
    CHECK(not_connected.load() == kReaders);
}
