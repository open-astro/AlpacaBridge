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
#include <alpacacore/vendor/zwo/zwo_caa_sdk.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/version.h>

#include <cmath>
#include <functional>
#include <variant>

#include "catch2_compat.h"
#include "rotator_sync_offset_test_dir.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Scripted CAA SDK: one rotator (id 7), single-threaded tests only. `degree` is
// the logical (reverse-applied) angle CAAGetDegree() reports, as in the driver.
class FakeCAASDK final : public alpacacore::vendor::zwo::ZWOCAASDK {
public:
    using Info = alpacacore::vendor::zwo::ZWOCAARotatorInfo;

    int id{7};
    std::string serial{"CAA-SN-1"};
    double degree{0.0};
    double max_degree{360.0};
    bool reverse{false};
    bool fail_info{false};
    int open_calls{0};
    int close_calls{0};
    double last_move_absolute{-1.0};
    double last_move_mechanical{-1.0};

    std::vector<Info> enumerate_rotators() override { return {Info{id, "CAA", max_degree}}; }
    bool get_rotator_info_by_id(int rotator_id, Info& info) override {
        if (fail_info || rotator_id != id) {
            return false;
        }
        info = Info{id, "CAA", max_degree};
        return true;
    }
    bool get_rotator_info_by_index(int, Info& info) override { return get_rotator_info_by_id(id, info); }
    void open_rotator(int) override { ++open_calls; }
    void close_rotator(int) override { ++close_calls; }
    alpacacore::vendor::zwo::ZWOCAAMotionStatus get_motion_status(int) override { return {}; }
    double get_degree(int) override { return degree; }
    void move_relative(int, double angle) override { degree += angle; }
    void move_absolute(int, double angle) override {
        last_move_absolute = angle;
        degree = angle;
    }
    void move_mechanical(int, double angle) override {
        last_move_mechanical = angle;
        degree = reverse ? 360.0 - angle : angle;
    }
    void stop(int) override {}
    void sync_degree(int, double angle) override { degree = angle; }
    double get_max_degree(int) override { return max_degree; }
    double get_temperature(int) override { return 20.0; }
    bool get_reverse(int) override { return reverse; }
    void set_reverse(int, bool value) override { reverse = value; }
    std::string get_serial_number(int) override { return serial; }
    std::string get_firmware_version(int) override { return "1.0"; }
    std::string get_rotator_type(int) override { return "CAA"; }
    std::string get_sdk_version() override { return "1, 7, 0, 0"; }
};

} // namespace

TEST_CASE("ZWO CAA Rotator Driver - Defaults", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "ZWO CAA");
    CHECK(driver->get_can_reverse() == true);
}

TEST_CASE("ZWO CAA Rotator Driver - Disconnected Behavior", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);

    REQUIRE(driver->get_connected() == false);
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

    require_alpaca_error([&]() { driver->get_reverse(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_reverse(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_mechanical_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_target_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_target_position(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_absolute(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move_mechanical(10.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->sync(10.0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO CAA Rotator Driver - Device metadata", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO CAA Rotator Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO CAA Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "ZWO_CAA_3");
}

TEST_CASE("ZWO CAA Rotator Driver - Device Number Assignment", "[zwo][rotator][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);
    auto driver5 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(5, 0);

    CHECK(driver0->get_device_number() == 0);
    CHECK(driver1->get_device_number() == 1);
    CHECK(driver5->get_device_number() == 5);
}

TEST_CASE("ZWO CAA Rotator Driver - Unique IDs", "[zwo][rotator][unit]") {
    auto driver0 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);
    auto driver1 = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0);

    CHECK(driver0->get_unique_id() != driver1->get_unique_id());
}

// Falsified by: zwo_rotator_driver.cpp get_step_size() dropping its
// ensure_connected() call, which would throw PropertyNotImplemented while
// disconnected instead of NotConnected. (No connected CAA fake exists, so the
// PropertyNotImplemented-when-connected half is code-reviewed only.)
TEST_CASE("ZWO CAA Rotator Driver - StepSize checks the connection first", "[zwo][rotator][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0);
    require_alpaca_error([&]() { (void)driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
}

// Falsified by: zwo_rotator_driver.cpp set_connected() dropping the
// RotatorSyncOffsetStore::load() after the serial is read (Position reads 10
// after the reconnect and in the new instance instead of 30).
TEST_CASE("ZWO CAA Rotator Driver - Sync offset survives reconnect and a new instance",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;

    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);
    driver->sync(30.0);
    CHECK(driver->get_position() == 30.0);

    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_position() == 30.0);

    auto second = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(1, 0, sdk);
    second->set_connected(true);
    CHECK(second->get_position() == 30.0);
    CHECK(second->get_mechanical_position() == 10.0);
}

// Falsified by: zwo_rotator_driver.cpp unique_id_locked() skipping its serial
// branch (a second rotator with another serial inherits the stored 30).
TEST_CASE("ZWO CAA Rotator Driver - Sync offset is keyed by serial number", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->sync(30.0);
    driver->set_connected(false);

    sdk.serial = "CAA-SN-2";
    auto other = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    other->set_connected(true);
    CHECK(other->get_position() == 10.0);
}

// Falsified by: zwo_rotator_driver.cpp get_mechanical_position() dropping the
// 360 - degree inversion for Reverse (reads 100 instead of 260).
TEST_CASE("ZWO CAA Rotator Driver - Reverse un-applies the SDK inversion", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 100.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_reverse() == false);
    CHECK(driver->get_mechanical_position() == 100.0);
    driver->set_reverse(true);
    CHECK(sdk.reverse == true);
    CHECK(driver->get_reverse() == true);
    CHECK(driver->get_mechanical_position() == 260.0);
}

// Falsified by: zwo_rotator_driver.cpp move_absolute() passing the logical
// target to the SDK instead of to_mechanical_angle(target) (SDK sees 50, not 30).
TEST_CASE("ZWO CAA Rotator Driver - Move applies the Sync offset", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->sync(30.0);  // offset 20

    driver->move_absolute(50.0);
    CHECK(sdk.last_move_absolute == 30.0);
    CHECK(driver->get_target_position() == 50.0);
    CHECK(driver->get_position() == 50.0);

    driver->move(10.0);  // relative to Position 50
    CHECK(sdk.last_move_absolute == 40.0);
    CHECK(driver->get_target_position() == 60.0);

    driver->move_absolute(370.0);  // wraps at max_degree
    CHECK(driver->get_target_position() == 10.0);
    require_alpaca_error([&]() { driver->move_absolute(std::nan("")); }, alpacacore::AlpacaError::InvalidValue);
}

// Falsified by: zwo_rotator_driver.cpp set_connected() dropping the
// close_rotator() in its catch(...) (open_calls 1, close_calls 0).
TEST_CASE("ZWO CAA Rotator Driver - Failed connect closes the handle", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.fail_info = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_connected() == false);
    CHECK(sdk.open_calls == 1);
    CHECK(sdk.close_calls == 1);
}
