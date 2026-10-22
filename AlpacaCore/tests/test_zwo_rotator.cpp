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
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/unique_id.h>
#include <alpacacore/vendor/zwo/zwo_caa_sdk.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_zwo_caa_sdk.h"
#include "getter_stall_probe.h"
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

using FakeCAASDK = alpacacore::test::FakeZWOCAASDK;

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
// disconnected instead of NotConnected. (Only the disconnected half is pinned
// here; the PropertyNotImplemented-when-connected half has no case.)
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

// Falsified by: zwo_rotator_driver.cpp unique_id_for() skipping its serial
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

// Decision 0008 (#1473): a Sync that cannot store its offset fails and keeps the old one.
TEST_CASE("ZWO CAA Rotator Driver - Sync that cannot save throws and keeps the offset",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->sync(30.0);
    REQUIRE(driver->get_position() == 30.0);

    const auto blocker = dir.file().parent_path().parent_path() / "blocker";
    { std::ofstream(blocker) << "x"; }
    alpacacore::util::RotatorSyncOffsetStore::set_default_path((blocker / "offsets.json").string());
    require_alpaca_error([&] { driver->sync(50.0); }, alpacacore::AlpacaError::DriverException);
    CHECK(driver->get_position() == 30.0);
}

// Decision 0018 (#1471): the serial-keyed offset moves to the assigned GUID at connect,
// which is the first moment the serial (and so the old key) is known.
TEST_CASE("ZWO CAA Rotator Driver - Connect adopts the offset saved under the serial-based ID",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    std::string legacy;
    {
        auto before_upgrade = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
        before_upgrade->set_connected(true);
        legacy = before_upgrade->get_unique_id();  // serial-based once connected
        before_upgrade->sync(30.0);
        before_upgrade->set_connected(false);
    }
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::load(legacy) == 20.0);  // offset = 30 - mechanical 10
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    const std::string guid = alpacacore::util::generate_unique_id();
    driver->assign_unique_id(guid);
    driver->set_connected(true);
    CHECK(driver->get_position() == 30.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 20.0);
    // Sync now saves under the GUID.
    driver->sync(45.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 35.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(legacy) == 0.0);
}

// A CAA without a serial saved its offset under an SDK-ID key, which names a
// slot, not a body. Only the router's call at the start that gives the stored
// entry its first GUID moves it; a connect never does, so a rotator added later
// cannot inherit it (decision 0018).
TEST_CASE("ZWO CAA Rotator Driver - Connect does not adopt an offset saved under an ID-based key",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    sdk.serial.clear();
    const std::string legacy = "ZWO_CAA_ID_" + std::to_string(sdk.id);
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save(legacy, 20.0));
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    const std::string guid = alpacacore::util::generate_unique_id();
    driver->assign_unique_id(guid);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 0.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(legacy) == 20.0);
}

TEST_CASE("ZWO CAA Rotator Driver - adopt_legacy_state moves an offset saved under an ID-based key",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    sdk.serial.clear();
    const std::string legacy = "ZWO_CAA_ID_" + std::to_string(sdk.id);
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save(legacy, 20.0));
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    const std::string guid = alpacacore::util::generate_unique_id();
    driver->assign_unique_id(guid);
    driver->adopt_legacy_state();
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == 20.0);
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(legacy) == 0.0);
    driver->set_connected(true);
    CHECK(driver->get_position() == 30.0);
}

// A client may connect before the router's upgrade-start adopt runs; the adopt
// then refreshes Position, not only the file (review of #1493).
TEST_CASE("ZWO CAA Rotator Driver - adopt_legacy_state while connected refreshes Position",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    sdk.serial.clear();
    REQUIRE(alpacacore::util::RotatorSyncOffsetStore::save("ZWO_CAA_ID_" + std::to_string(sdk.id), 20.0));
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->assign_unique_id(alpacacore::util::generate_unique_id());
    driver->set_connected(true);
    REQUIRE(driver->get_position() == 10.0);
    driver->adopt_legacy_state();
    CHECK(driver->get_position() == 30.0);
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

// open-astro#294: DeviceState and the position getters used to round-trip the
// device once per property. One refill now serves them all for a short TTL.
namespace {
void wait_past_status_ttl() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
}  // namespace

// Falsified by: zwo_rotator_driver.cpp read_status() calling the SDK on every
// read instead of through status_cache_.get() (degree_calls 3, not 1).
TEST_CASE("ZWO CAA Rotator Driver - DeviceState is one device read, served from the TTL cache",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.motion_calls = sdk.degree_calls = sdk.reverse_calls = 0;

    CHECK(driver->get_device_state().size() == 4);  // IsMoving, MechanicalPosition, Position, TimeStamp
    CHECK(sdk.motion_calls == 1);
    CHECK(sdk.degree_calls == 1);
    CHECK(sdk.reverse_calls == 0);

    (void)driver->get_device_state();
    CHECK(sdk.degree_calls == 1);  // inside the TTL: no device I/O

    wait_past_status_ttl();
    (void)driver->get_device_state();
    CHECK(sdk.motion_calls == 2);
    CHECK(sdk.degree_calls == 2);
}

// Falsified by: zwo_rotator_driver.cpp get_reverse() calling sdk_.get_reverse()
// instead of returning reverse_ (reverse_calls 3, not 0).
TEST_CASE("ZWO CAA Rotator Driver - Reverse is read once at connect, not per getter",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.reverse = true;
    sdk.degree = 100.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.reverse_calls = 0;

    CHECK(driver->get_reverse() == true);
    CHECK(driver->get_mechanical_position() == 260.0);
    CHECK(driver->get_mechanical_position() == 260.0);
    CHECK(sdk.reverse_calls == 0);

    // Disconnect clears it; the reconnect reads the device again.
    driver->set_connected(false);
    sdk.reverse = false;
    driver->set_connected(true);
    CHECK(sdk.reverse_calls == 1);
    CHECK(driver->get_reverse() == false);
}

// Falsified by: zwo_rotator_driver.cpp move_absolute() dropping its
// status_cache_.invalidate() (Position keeps the pre-move 10 for a TTL).
TEST_CASE("ZWO CAA Rotator Driver - A move invalidates the cached frame", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 10.0);
    driver->move_absolute(90.0);
    CHECK(driver->get_position() == 90.0);
}

// Falsified by: ttl_status_cache.h get() dropping its health_.faulted() throw
// (the third failure reports the raw SDK error, not "communications compromised").
TEST_CASE("ZWO CAA Rotator Driver - A dead link refuses reads instead of serving the cache",
          "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);

    sdk.fail_reads = true;
    wait_past_status_ttl();
    for (int i = 0; i < 2; ++i) {
        CHECK_THROWS(driver->get_position());
    }
    // Third consecutive failure latches the fault: the message names it.
    try {
        (void)driver->get_position();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_device_state().size() == 1);  // TimeStamp only

    // Reverse is a writable setting held in memory: it refuses too while latched.
    try {
        (void)driver->get_reverse();
        FAIL("expected a throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }

    // Recovery needs no reconnect.
    sdk.fail_reads = false;
    CHECK(driver->get_position() == 10.0);
    CHECK(driver->get_reverse() == false);
}

// Falsified by: zwo_rotator_driver.cpp sync() dropping its pre-read
// status_cache_.invalidate() (the offset is taken against the cached 10, giving 40).
TEST_CASE("ZWO CAA Rotator Driver - Sync takes its offset from the live angle", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);
    sdk.degree = 50.0;
    driver->sync(0.0);
    wait_past_status_ttl();  // the offset must hold against the live angle, not the cached frame
    CHECK(driver->get_position() == 0.0);
}

// Falsified by: zwo_rotator_driver.cpp set_reverse() dropping its
// status_cache_.invalidate() (Position keeps the pre-reverse 10).
TEST_CASE("ZWO CAA Rotator Driver - Reverse change drops the cached frame", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);
    sdk.degree = 350.0;
    driver->set_reverse(true);
    CHECK(driver->get_position() == 350.0);
}

// Falsified by: zwo_rotator_driver.cpp move() dropping its pre-read
// status_cache_.invalidate() (the target is built from the cached 10, giving 20).
TEST_CASE("ZWO CAA Rotator Driver - Relative move starts from the live angle", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 10.0);
    sdk.degree = 50.0;
    driver->move(10.0);
    CHECK(sdk.last_move_absolute == 60.0);
}

// Falsified by: zwo_rotator_driver.cpp halt() dropping its
// status_cache_.invalidate() (IsMoving keeps answering true for a TTL).
TEST_CASE("ZWO CAA Rotator Driver - Halt drops the cached frame", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.moving = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_is_moving() == true);
    driver->halt();
    CHECK(driver->get_is_moving() == false);
}

// Falsified by: zwo_rotator_driver.cpp move_mechanical() dropping its
// status_cache_.invalidate() (Position keeps the pre-move angle for a TTL).
TEST_CASE("ZWO CAA Rotator Driver - A mechanical move drops the cached frame", "[zwo][rotator][unit][fake-sdk]") {
    alpacacore::test::TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.degree = 10.0;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 10.0);
    driver->move_mechanical(120.0);
    CHECK(driver->get_position() == 120.0);
}

// Decision 0016: Name and UniqueID need no device access, so a connect that holds the SDK open must not park them.
// Falsified by: zwo_rotator_driver.cpp set_connected(true) holding mutex_ across open_rotator() (get_name() then
// waits the whole 300 ms open).
TEST_CASE("ZWO CAA Rotator Driver - Name and UniqueID answer while a connect holds the SDK open",
          "[zwo][rotator][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    sdk.open_delay = kHoldOpen;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);

    driver->connect();
    REQUIRE(wait_for_flag(sdk.open_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(driver->get_connecting());  // the open is still in flight, so the timings above are not vacuous

    REQUIRE(wait_until_connected(*driver));
    CHECK(driver->get_name() == "CAA");
    CHECK(driver->get_unique_id() == "ZWO_CAA_SN_CAA-SN-1");
    driver->set_connected(false);
}

// Falsified by: zwo_rotator_driver.cpp get_name() or get_unique_id() taking io_mutex_ (the identity read then waits
// the 300 ms status refill).
TEST_CASE("ZWO CAA Rotator Driver - Identity answers while a status read is in flight",
          "[zwo][rotator][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.read_delay = kHoldOpen;

    std::thread reader([&] {
        try {
            (void)driver->get_is_moving();
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.read_entered));

    CHECK(time_call([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(time_call([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(time_call([&] { CHECK(driver->get_can_reverse()); }) < kGetterBudget);
    CHECK(sdk.busy.load() > 0);  // the refill is still in flight

    reader.join();
    driver->set_connected(false);
}

// A disconnect must wait for the exchange in flight, never close the handle under it (the I/O lock).
// Falsified by: zwo_rotator_driver.cpp set_connected(false) calling close_rotator() without io_mutex_, or
// move_absolute()/move()/move_mechanical()/set_reverse()/halt() reading the id and then calling the SDK without
// it (closed_while_busy is set; one section per write).
TEST_CASE("ZWO CAA Rotator Driver - A disconnect waits for the exchange in flight instead of closing under it",
          "[zwo][rotator][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    std::thread worker;
    SECTION("a status read") {
        sdk.read_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                (void)driver->get_is_moving();
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.read_entered));
    }
    SECTION("a move") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->move_absolute(90.0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }
    SECTION("a mechanical move") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->move_mechanical(90.0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }
    SECTION("a relative move") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->move(45.0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }
    SECTION("a reverse write") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->set_reverse(true);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }
    SECTION("a halt") {
        sdk.move_delay = kHoldOpen;
        worker = std::thread([&] {
            try {
                driver->halt();
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
    }

    CHECK(time_call([&] { driver->set_connected(false); }) < std::chrono::seconds(5));
    worker.join();
    CHECK_FALSE(sdk.closed_while_busy.load());
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.close_calls == 1);
}

// A disconnect that wins the I/O lock while status reads queue behind a write is not a link failure: every reader
// gets NotConnected, none the "communications compromised" fault that three counted failures would latch.
// Falsified by: zwo_rotator_driver.cpp read_status() throwing NotConnected inside the cache refill instead of returning
// the closed frame (the third queued reader then gets DriverException "communications compromised").
TEST_CASE("ZWO CAA Rotator Driver - A disconnect during queued status reads is not a link failure",
          "[zwo][rotator][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    TempSyncOffsetDir dir;
    FakeCAASDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    sdk.move_delay = kHoldOpen;
    std::thread mover([&] {
        try {
            driver->move_absolute(90.0);
        } catch (const std::exception&) {
        }
    });
    REQUIRE(wait_for_flag(sdk.move_entered));

    constexpr int kReaders = 8;
    std::atomic<int> not_connected{0};
    std::atomic<int> other{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            try {
                (void)driver->get_is_moving();
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
