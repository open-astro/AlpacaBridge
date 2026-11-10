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
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../src/vendors/zwo/zwo_fields.h"
#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_sdk.h"
#include "getter_stall_probe.h"

namespace {

using alpacacore::test::require_alpaca_error;

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

    require_alpaca_error([&] { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { driver->command_blind("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_string("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
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

// Past the driver's 100 ms heater read TTL on the fake clock.
void advance_past_status_ttl(alpacacore::test::FakeTaskClock& clock) { clock.advance(std::chrono::milliseconds(150)); }

}  // namespace

TEST_CASE("ZWO Dew Heater Switch Driver - DeviceState reads the camera once per status frame", "[zwo][switch][unit]") {
    alpacacore::test::FakeTaskClock clock;  // first: it and the SDK outlive the driver
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    CHECK(driver->get_switch_value(0) == 40.0);
    CHECK(driver->get_switch(0));
    // Before the cache each of these was its own SDK read.
    CHECK(sdk.reads == 1);

    const auto state = driver->get_device_state();
    CHECK_FALSE(state.empty());
    CHECK(sdk.reads == 1);

    advance_past_status_ttl(clock);
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

// Falsified by: zwo_switch_driver.cpp set_switch_value, call set_control_value outside with_link (the fake refuses it)
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
    alpacacore::test::FakeTaskClock clock;  // first: it and the SDK outlive the driver
    FakeDewSdk sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk, clock);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(0) == 40.0);

    sdk.fail_reads = true;
    advance_past_status_ttl(clock);
    for (int i = 0; i < 3; ++i) {
        // Two failed reads ("Failed to read dew heater value"), then the latch: all DriverException.
        require_alpaca_error([&] { (void)driver->get_switch_value(0); }, alpacacore::AlpacaError::DriverException);
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

// Falsified by: zwo_switch_driver.cpp list_dew_heater_cameras, read the caps and serial outside with_link (the fake
// refuses them, so no camera lists the serial)
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
// Falsified by: zwo_switch_driver.cpp set_connected(true) holding mutex_ across open_camera_link() (get_unique_id()
// then waits the whole 300 ms open).
TEST_CASE("ZWO Dew Heater Switch Driver - Name and UniqueID answer while a connect holds the SDK open",
          "[zwo][switch][unit][getter-stall]") {
    using namespace alpacacore::test;
    FakeDewSdk sdk;
    sdk.open_delay_ms = static_cast<int>(kHoldOpen.count());
    auto driver = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, 3, sdk);

    driver->connect();
    REQUIRE(wait_for_flag(sdk.open_entered));

    CHECK(timed([&] { (void)driver->get_name(); }) < kGetterBudget);
    CHECK(timed([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
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

    CHECK(timed([&] { (void)driver->get_unique_id(); }) < kGetterBudget);
    CHECK(timed([&] { CHECK(driver->get_switch_name(0) == "DewHeater"); }) < kGetterBudget);
    CHECK(timed([&] { (void)driver->get_switch_description(0); }) < kGetterBudget);
    CHECK(timed([&] { CHECK(driver->get_max_switch_value(0) == 100.0); }) < kGetterBudget);
    CHECK(sdk.busy.load() > 0);  // the refill is still in flight

    reader.join();
    driver->set_connected(false);
}

// A disconnect must wait for the exchange in flight, never close the camera under it (the I/O lock).
// Falsified by: zwo_switch_driver.cpp set_connected(false) calling close_camera_link() without io_mutex_, or
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

    CHECK(timed([&] { driver->set_connected(false); }) < std::chrono::seconds(5));
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

// ---- The dew heater and the camera driver share one open (issue #1667) ----

namespace {

using alpacacore::test::FakeZWOSDK;
using alpacacore::vendor::zwo::ZWOSdkDewHeaterSdk;

// Camera driver and dew heater switch on one fake camera, as one process holds them.
struct SharedCamera {
    FakeZWOSDK sdk;
    ZWOSdkDewHeaterSdk dew_sdk{sdk};
    std::unique_ptr<alpacacore::CameraDriver> camera =
        alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    std::unique_ptr<alpacacore::SwitchDriver> heater =
        alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, sdk.camera.camera_id, dew_sdk);
};

}  // namespace

// Falsified by: the camera driver or the switch closing the camera's one real open while the other still holds it.
TEST_CASE("ZWO camera and dew heater - one real open, closed by the last holder", "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.camera->set_connected(true);
    rig.heater->set_connected(true);
    CHECK(rig.sdk.holders(rig.sdk.camera.camera_id) == 2);
    CHECK(rig.sdk.open_count() == 1);
    CHECK(rig.sdk.call_count("init_camera") == 1);

    rig.camera->set_connected(false);  // the camera disconnects first: the switch still reads
    CHECK(rig.sdk.open_count() == 1);
    CHECK(rig.heater->get_switch_value(0) == Catch::Approx(0.0));

    rig.heater->set_connected(false);
    CHECK(rig.sdk.open_count() == 0);
}

// Falsified by: as above, for the reverse order (the switch closes the open under the camera).
TEST_CASE("ZWO dew heater and camera - the switch disconnects first and the camera keeps its handle",
          "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.heater->set_connected(true);
    rig.camera->set_connected(true);

    rig.heater->set_connected(false);
    CHECK(rig.sdk.open_count() == 1);
    CHECK(rig.camera->get_ccd_temperature() == Catch::Approx(15.0));

    rig.camera->set_connected(false);
    CHECK(rig.sdk.open_count() == 0);
}

// Falsified by: shared_open_table.h call() not retiring the entry on a loss code from the camera's call, so the
// switch's next read reaches the SDK on the dead handle.
TEST_CASE("ZWO camera and dew heater - a camera removal error stops the switch before the SDK",
          "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.camera->set_connected(true);
    rig.heater->set_connected(true);

    rig.sdk.removed = true;  // ASI_ERROR_CAMERA_REMOVED on the camera's next call
    require_alpaca_error([&] { (void)rig.camera->get_ccd_temperature(); }, alpacacore::AlpacaError::NotConnected);
    CHECK(rig.sdk.holders(rig.sdk.camera.camera_id) == 0);
    CHECK(rig.sdk.open_count() == 0);  // the dead handle is closed at once

    rig.sdk.removed = false;  // a read that reached the SDK now gets the fake's "not open" DriverException
    const int touches = rig.sdk.unit_touches();
    try {
        (void)rig.heater->get_switch_value(0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
        CHECK(std::string(ex.what()).find("removed from USB") != std::string::npos);
    }
    CHECK(rig.sdk.unit_touches() == touches);  // refused by the table, before the SDK
}

// ---- A camera loss and replug with the dew heater on the same open (issue #1667, decision 0009) ----

namespace {

using alpacacore::AlpacaError::NotConnected;

// Both connected on one id; then the camera's temperature read meets the pulled cable.
struct SharedCameraLost : SharedCamera {
    SharedCameraLost() {
        camera->set_connected(true);
        heater->set_connected(true);
        sdk.removed = true;
        require_alpaca_error([&] { (void)camera->get_ccd_temperature(); }, NotConnected);
    }
};

// The camera comes back on @p new_id with the same serial and reconnects; the switch still holds its link from
// before the loss.
void check_stale_switch_after_camera_reconnect(int new_id) {
    {
        SharedCameraLost rig;
        rig.sdk.replug({{new_id, "FAKE-ZWO-17"}});
        rig.camera->set_connected(false);  // a lost camera reads as disconnected: completes at once
        rig.camera->set_connected(true);
        REQUIRE(rig.camera->get_connected());
        CHECK(rig.sdk.holders(new_id) == 1);

        // The switch's link names the handle that died: refused before the SDK, the camera's new open untouched, and
        // the switch now reads the loss.
        const int touches = rig.sdk.unit_touches();
        require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, NotConnected);
        CHECK(rig.sdk.unit_touches() == touches);
        CHECK_FALSE(rig.heater->get_connected());
        CHECK(rig.sdk.holders(new_id) == 1);
    }
    {
        // The switch has not called since the loss, so its Disconnect closes on the link that died: that stale
        // close must not close what the camera reopened.
        SharedCameraLost rig;
        rig.sdk.replug({{new_id, "FAKE-ZWO-17"}});
        rig.camera->set_connected(false);
        rig.camera->set_connected(true);
        REQUIRE(rig.camera->get_connected());

        rig.heater->set_connected(false);
        CHECK(rig.sdk.open_count() == 1);
        CHECK(rig.camera->get_ccd_temperature() == Catch::Approx(15.0));
        rig.camera->set_connected(false);
        CHECK(rig.sdk.open_count() == 0);
    }
}

}  // namespace

// Falsified by: zwo_camera_driver.cpp on_link() not latching the loss (the camera stays Connected), or
// shared_open_table.h call() running the switch's call on the retired open (the call reaches the unit).
TEST_CASE("ZWO camera loss and dew heater - the switch gets NotConnected before the SDK is touched",
          "[zwo][switch][camera][unit][link]") {
    SharedCameraLost rig;
    CHECK_FALSE(rig.camera->get_connected());
    CHECK(rig.sdk.open_count() == 0);  // the dead handle is closed

    const int touches = rig.sdk.unit_touches();
    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, NotConnected);
    require_alpaca_error([&] { rig.heater->set_switch_value(0, 10.0); }, NotConnected);
    CHECK(rig.sdk.unit_touches() == touches);
}

// Falsified by: SharedOpenTable::close() closing a newer open of the id on a stale link (the camera loses the handle
// it reopened), or the switch's pre-loss link passing the check on the reopened id.
TEST_CASE("ZWO camera reconnect on the same id - a stale close from the switch does not close the new handle",
          "[zwo][switch][camera][unit][link]") {
    check_stale_switch_after_camera_reconnect(17);
}

// Falsified by: zwo_camera_driver.cpp resolve_camera_id() binding the configured id after the first connect (the
// Connect fails with "ZWO camera ID 17 not found").
TEST_CASE("ZWO camera reconnect on a new id - a stale close from the switch does not close the new handle",
          "[zwo][switch][camera][unit][link]") {
    check_stale_switch_after_camera_reconnect(23);
}

// Falsified by: zwo_camera_driver.cpp on_link() not latching the loss when the table refuses its stale link (the
// camera stays Connected after the switch met the removal), or a camera call that skips the link check.
TEST_CASE("ZWO dew heater meets the loss first - the camera gets NotConnected before the SDK is touched",
          "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.camera->set_connected(true);
    rig.heater->set_connected(true);
    rig.sdk.removed = true;
    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, NotConnected);

    const int touches = rig.sdk.unit_touches();
    require_alpaca_error([&] { (void)rig.camera->get_ccd_temperature(); }, NotConnected);
    CHECK(rig.sdk.unit_touches() == touches);
    CHECK_FALSE(rig.camera->get_connected());

    CHECK_FALSE(rig.heater->get_connected());  // the switch met the loss: it reads disconnected too

    // The cable is back: the camera reconnects, and the switch reads again after its own Disconnect and Connect,
    // through the camera's new open.
    rig.sdk.replug({{17, "FAKE-ZWO-17"}});
    rig.camera->set_connected(true);
    rig.heater->set_connected(false);
    rig.heater->set_connected(true);
    CHECK(rig.sdk.holders(17) == 2);
    CHECK(rig.heater->get_switch_value(0) == Catch::Approx(0.0));
    rig.heater->set_connected(false);
    rig.camera->set_connected(false);
    CHECK(rig.sdk.open_count() == 0);
}

// ---- The dew heater reads the camera's loss (issue #1667, decision 0009 rules 3 and 5) ----

// Falsified by: zwo_switch_driver.cpp get_switch_value() not calling note_loss_locked() on a NotConnected from its
// read; set_connected(false) on a lost switch keeping the loss reason.
// The camera met the removal first: the switch's next read finds its open retired and reads Connected false from
// then on, with the reason in LinkFault until its own Disconnect; its Connect after the replug opens afresh.
TEST_CASE("ZWO dew heater - a camera loss reads Connected false on the switch", "[zwo][switch][camera][unit][link]") {
    SharedCameraLost rig;
    REQUIRE(rig.heater->get_connected());  // nothing told the switch yet

    try {
        (void)rig.heater->get_switch_value(0);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == NotConnected);
        CHECK(std::string(ex.what()).find("removed from USB") != std::string::npos);
    }
    CHECK_FALSE(rig.heater->get_connected());
    CHECK(rig.heater->get_device_state().empty());
    CHECK(rig.heater->get_link_fault().find("removed from USB") != std::string::npos);
    require_alpaca_error([&] { (void)rig.heater->get_max_switch_value(0); }, NotConnected);

    rig.heater->set_connected(false);  // a lost switch reads as disconnected: completes and clears the reason
    CHECK(rig.heater->get_link_fault().empty());
    CHECK(rig.sdk.open_count() == 0);

    rig.sdk.replug({{17, "FAKE-ZWO-17"}});
    rig.heater->set_connected(true);
    REQUIRE(rig.heater->get_connected());
    CHECK(rig.heater->get_switch_value(0) == Catch::Approx(0.0));
    rig.heater->set_connected(false);
    CHECK(rig.sdk.open_count() == 0);
}

// Falsified by: zwo_switch_driver.cpp get_switch_value() not calling note_loss_locked(), or shared_open_table.h
// call(id, fn, failed) skipping the failed() check.
// A bare pull met by the switch first: its heater read is not readable on a camera that no longer enumerates, so the
// switch and then the camera read the loss, and the camera's call never reaches the SDK.
TEST_CASE("ZWO dew heater - a bare cable pull met by the switch drops the switch and the camera",
          "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.camera->set_connected(true);
    rig.heater->set_connected(true);
    rig.sdk.pulled = true;

    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, NotConnected);
    CHECK_FALSE(rig.heater->get_connected());
    CHECK(rig.sdk.open_count() == 0);

    const int touches = rig.sdk.unit_touches();
    require_alpaca_error([&] { (void)rig.camera->get_ccd_temperature(); }, NotConnected);
    CHECK(rig.sdk.unit_touches() == touches);
    CHECK_FALSE(rig.camera->get_connected());
}

// Falsified by: zwo_switch_driver.cpp set_switch_value() not calling note_loss_locked() on a NotConnected from its
// write.
// A heater write that meets the pull is the same loss as a read.
TEST_CASE("ZWO dew heater - a heater write that meets a bare cable pull drops the switch",
          "[zwo][switch][camera][unit][link]") {
    SharedCamera rig;
    rig.heater->set_connected(true);
    rig.sdk.pulled = true;

    require_alpaca_error([&] { rig.heater->set_switch_value(0, 10.0); }, NotConnected);
    CHECK_FALSE(rig.heater->get_connected());
    CHECK(rig.heater->get_link_fault().find("removed from USB") != std::string::npos);
    CHECK(rig.sdk.open_count() == 0);
}

// ---- The dew heater's own fault latch and the staleness bound (decision 0009 rules 1 and 2b) ----

namespace {

// The camera and the switch on one fake camera, the switch's heater read cache on a fake clock.
struct ClockedSharedCamera {
    alpacacore::test::FakeTaskClock clock;  // first: it and the SDK outlive the drivers
    FakeZWOSDK sdk;
    ZWOSdkDewHeaterSdk dew_sdk{sdk};
    std::unique_ptr<alpacacore::CameraDriver> camera =
        alpacacore::vendor::zwo::create_zwo_camera(0, sdk.camera.camera_id, sdk);
    std::unique_ptr<alpacacore::SwitchDriver> heater =
        alpacacore::vendor::zwo::create_zwo_dew_heater_switch(0, sdk.camera.camera_id, dew_sdk, clock);

    ClockedSharedCamera() {
        camera->set_connected(true);
        heater->set_connected(true);
    }

    // Three failed heater reads, one TTL apart: the cache latches its fault.
    void latch_heater_fault() {
        sdk.fail_heater_read = true;
        for (int i = 0; i < 3; ++i) {
            clock.advance(std::chrono::milliseconds(150));
            require_alpaca_error([&] { (void)heater->get_switch_value(0); }, alpacacore::AlpacaError::DriverException);
        }
    }
};

}  // namespace

// Falsified by: zwo_switch_driver.cpp get_link_fault() returning only the loss reason (the latched heater fault
// reads empty); set_switch_value() not refusing a write while the fault is latched.
TEST_CASE("ZWO dew heater - LinkFault reports the latched heater read fault", "[zwo][switch][camera][unit][link]") {
    ClockedSharedCamera rig;
    REQUIRE(rig.heater->get_link_fault().empty());

    rig.latch_heater_fault();

    CHECK(rig.heater->get_connected());  // a fault, not a loss: the camera still enumerates
    CHECK_FALSE(rig.heater->get_link_fault().empty());
    rig.clock.advance(std::chrono::milliseconds(150));
    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, alpacacore::AlpacaError::DriverException);
    const int writes = rig.sdk.call_count("set_control_value");
    require_alpaca_error([&] { rig.heater->set_switch_value(0, 1.0); }, alpacacore::AlpacaError::DriverException);
    CHECK(rig.sdk.call_count("set_control_value") == writes);  // refused before the SDK

    rig.sdk.fail_heater_read = false;  // the next good read clears it
    rig.clock.advance(std::chrono::milliseconds(150));
    CHECK(rig.heater->get_switch_value(0) == Catch::Approx(0.0));
    CHECK(rig.heater->get_link_fault().empty());
}

// Falsified by: zwo_switch_driver.cpp get_switch_value() not calling lose_link_if_fault_stale_locked() after a
// failed read (the fault stands for ever with Connected true); set_connected(false) on a lost switch not resetting
// the cache (LinkFault keeps the latched fault after the Disconnect).
TEST_CASE("ZWO dew heater - a heater fault standing past the staleness bound loses the link",
          "[zwo][switch][camera][unit][link]") {
    ClockedSharedCamera rig;
    rig.latch_heater_fault();

    rig.clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, alpacacore::AlpacaError::DriverException);
    CHECK(rig.heater->get_connected());  // inside the bound: still a fault

    rig.clock.advance(std::chrono::seconds(2));
    require_alpaca_error([&] { (void)rig.heater->get_switch_value(0); }, NotConnected);
    CHECK_FALSE(rig.heater->get_connected());
    CHECK(rig.heater->get_device_state().empty());
    CHECK(rig.heater->get_link_fault().find("did not answer") != std::string::npos);
    // Only the switch's open is dropped: the camera still holds the camera and reads it.
    CHECK(rig.sdk.holders(rig.sdk.camera.camera_id) == 1);
    CHECK(rig.camera->get_connected());
    CHECK(rig.camera->get_ccd_temperature() == Catch::Approx(15.0));

    // The client's Connect brings it back once the heater answers; nothing reopened on its own.
    rig.sdk.fail_heater_read = false;
    rig.heater->set_connected(false);
    CHECK(rig.heater->get_link_fault().empty());
    rig.heater->set_connected(true);
    REQUIRE(rig.heater->get_connected());
    CHECK(rig.heater->get_switch_value(0) == Catch::Approx(0.0));
}
