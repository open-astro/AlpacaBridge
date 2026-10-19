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
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

#include "catch2_compat.h"
#include "fake_zwo_caa_sdk.h"
#include "fake_zwo_eaf_sdk.h"
#include "fake_zwo_efw_sdk.h"
#include "locked_zwo_caa_sdk.h"
#include "locked_zwo_eaf_sdk.h"
#include "locked_zwo_efw_sdk.h"

// The EFW, EAF and CAA SDK seams (issue #1309): each driver runs over a scripted fake, directly and
// behind the Locked decorator that serialises every SDK call.

namespace {

namespace zwo = alpacacore::vendor::zwo;
using alpacacore::AlpacaException;
using alpacacore::test::FakeZWOCAASDK;
using alpacacore::test::FakeZWOEAFSDK;
using alpacacore::test::FakeZWOEFWSDK;
using alpacacore::test::LockedZWOCAASDK;
using alpacacore::test::LockedZWOEAFSDK;
using alpacacore::test::LockedZWOEFWSDK;

int error_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

}  // namespace

// Falsified by: zwo_filterwheel_driver.cpp create_zwo_efw_filterwheel_by_index() passing
// ZWOEFWSDKWrapper::instance() instead of the injected sdk (the fake sees no open, and the real
// singleton is touched).
TEST_CASE("ZWO EFW seam - driver connects over the injected SDK and moves the wheel",
          "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(sdk.open_calls == 1);
    CHECK(driver->get_names().size() == 7);

    driver->set_position(4);
    CHECK(sdk.last_set_position == 4);
    // moving_reads is 0, so the fake settles at once, which hardware never does; the moving
    // path is pinned by "ZWO EFW seam - Position reads -1 while the wheel moves".
    CHECK(driver->get_position() == 4);

    driver->set_connected(false);
    CHECK(sdk.close_calls == 1);
}

// Falsified by: zwo_filterwheel_driver.cpp get_position() mapping the SDK's -1 to slot 0 (Position
// reads 0, not the ASCOM moving sentinel).
TEST_CASE("ZWO EFW seam - Position reads -1 while the wheel moves", "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    sdk.moving_reads = 2;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->set_position(4);
    CHECK(driver->get_position() == -1);
    CHECK(error_code([&] { driver->set_position(2); }) == alpacacore::AlpacaError::InvalidOperation);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == 4);
    CHECK(sdk.moving_refusals == 1);

    driver->set_position(2);
    CHECK(sdk.last_set_position == 2);
}

// Falsified by: zwo_filterwheel_driver.cpp get_device_sdk_version() reading the wrapper singleton
// instead of sdk_ (the version row is empty, not 1.7.0.0).
TEST_CASE("ZWO EFW seam - SDK version comes from the injected SDK", "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    REQUIRE(driver->get_device_sdk_version().has_value());
    CHECK(*driver->get_device_sdk_version() == "1.7.0.0");
}

// Falsified by: zwo_filterwheel_driver.cpp resolve_wheel_id_locked() dropping its empty-enumeration
// throw (the connect then continues with a bogus id and no NotConnected).
TEST_CASE("ZWO EFW seam - no wheel attached refuses the connect with NotConnected",
          "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    sdk.no_wheels = true;
    auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);

    CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_calls == 0);
}

// Falsified by: zwo_filterwheel_driver.cpp set_connected() storing connected_ before open_wheel()
// returns (get_connected() reads true after the scripted open failure).
TEST_CASE("ZWO EFW seam - a failed connect leaves the wheel closed", "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    sdk.fail_open = true;
    auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);

    CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(sdk.open_calls == 0);
}

// Falsified by: locked_zwo_efw_sdk.h set_position() forwarding wheel_id as the slot (get_position reads
// 5, not 3). Every one of the EFW forwards has an assertion that fails when it is broken.
TEST_CASE("FakeZWOEFWSDK, FakeZWOEAFSDK and FakeZWOCAASDK behind their Locked decorators - forward every call",
          "[zwo][unit][fake-sdk]") {
    FakeZWOEFWSDK efw;
    efw.hardware_error_code = 7;
    LockedZWOEFWSDK locked_efw(efw);
    zwo::ZWOEFWInfo wheel;
    REQUIRE(locked_efw.enumerate_wheels().size() == 1);
    REQUIRE(locked_efw.get_wheel_info_by_id(efw.id, wheel));
    CHECK(wheel.slot_count == 7);
    zwo::ZWOEFWInfo by_index;
    REQUIRE(locked_efw.get_wheel_info_by_index(0, by_index));
    CHECK(by_index.wheel_id == efw.id);
    locked_efw.open_wheel(efw.id);
    locked_efw.set_position(efw.id, 3);
    CHECK(locked_efw.get_position(efw.id) == 3);
    CHECK_FALSE(locked_efw.get_unidirectional(efw.id));
    locked_efw.set_unidirectional(efw.id, true);
    CHECK(locked_efw.get_unidirectional(efw.id));
    locked_efw.calibrate(efw.id);
    CHECK(efw.calibrate_calls == 1);
    CHECK(locked_efw.get_hardware_error_code(efw.id) == 7);
    CHECK(locked_efw.get_serial_number(efw.id) == "EFW-SN-1");
    CHECK(locked_efw.get_firmware_version(efw.id) == "1.0.0");
    CHECK(locked_efw.get_sdk_version() == "1, 7, 0, 0");
    locked_efw.close_wheel(efw.id);
    CHECK(efw.open_calls == 1);
    CHECK(efw.close_calls == 1);

    FakeZWOEAFSDK eaf;
    LockedZWOEAFSDK locked_eaf(eaf);
    REQUIRE(locked_eaf.enumerate_focusers().size() == 1);
    FakeZWOEAFSDK::Info eaf_info;
    REQUIRE(locked_eaf.get_focuser_info_by_id(eaf.id, eaf_info));
    CHECK(eaf_info.focuser_id == eaf.id);
    CHECK_FALSE(locked_eaf.get_focuser_info_by_id(eaf.id + 1, eaf_info));
    FakeZWOEAFSDK::Info eaf_by_index;
    REQUIRE(locked_eaf.get_focuser_info_by_index(0, eaf_by_index));
    CHECK(eaf_by_index.focuser_id == eaf.id);
    locked_eaf.open_focuser(eaf.id);
    CHECK(eaf.open_calls == 1);
    CHECK_FALSE(locked_eaf.is_moving(eaf.id));
    eaf.moving = true;
    CHECK(locked_eaf.is_moving(eaf.id));
    locked_eaf.stop(eaf.id);
    CHECK_FALSE(eaf.moving);
    locked_eaf.move(eaf.id, 321);
    CHECK(locked_eaf.get_position(eaf.id) == 321);
    locked_eaf.set_max_step(eaf.id, 40000);
    CHECK(eaf.last_set_max_step == 40000);
    CHECK(locked_eaf.get_max_step(eaf.id) == 40000);
    CHECK(locked_eaf.get_step_range(eaf.id) == 1000);
    CHECK(locked_eaf.get_temperature(eaf.id) == 12.5);
    CHECK(locked_eaf.get_serial_number(eaf.id) == "EAF-SN-1");
    CHECK(locked_eaf.get_firmware_version(eaf.id) == "1.0");
    CHECK(locked_eaf.get_sdk_version() == "1, 7, 0, 0");
    locked_eaf.close_focuser(eaf.id);
    CHECK(eaf.close_calls == 1);

    FakeZWOCAASDK caa;
    LockedZWOCAASDK locked_caa(caa);
    REQUIRE(locked_caa.enumerate_rotators().size() == 1);
    FakeZWOCAASDK::Info caa_info;
    REQUIRE(locked_caa.get_rotator_info_by_id(caa.id, caa_info));
    CHECK(caa_info.rotator_id == caa.id);
    CHECK_FALSE(locked_caa.get_rotator_info_by_id(caa.id + 1, caa_info));
    FakeZWOCAASDK::Info caa_by_index;
    REQUIRE(locked_caa.get_rotator_info_by_index(0, caa_by_index));
    CHECK(caa_by_index.rotator_id == caa.id);
    locked_caa.open_rotator(caa.id);
    CHECK(caa.open_calls == 1);
    locked_caa.move_absolute(caa.id, 45.0);
    CHECK(locked_caa.get_degree(caa.id) == 45.0);
    CHECK(caa.last_move_absolute == 45.0);
    locked_caa.move_relative(caa.id, 5.0);
    CHECK(locked_caa.get_degree(caa.id) == 50.0);
    locked_caa.move_mechanical(caa.id, 90.0);
    CHECK(caa.last_move_mechanical == 90.0);
    locked_caa.sync_degree(caa.id, 12.0);
    CHECK(caa.degree == 12.0);
    CHECK(locked_caa.get_max_degree(caa.id) == 360.0);
    CHECK(locked_caa.get_temperature(caa.id) == 20.0);
    CHECK_FALSE(locked_caa.get_reverse(caa.id));
    locked_caa.set_reverse(caa.id, true);
    CHECK(locked_caa.get_reverse(caa.id));
    CHECK_FALSE(locked_caa.get_motion_status(caa.id).is_moving);
    caa.moving = true;
    CHECK(locked_caa.get_motion_status(caa.id).is_moving);
    locked_caa.stop(caa.id);
    CHECK_FALSE(caa.moving);
    CHECK(locked_caa.get_serial_number(caa.id) == "CAA-SN-1");
    CHECK(locked_caa.get_firmware_version(caa.id) == "1.0");
    CHECK(locked_caa.get_rotator_type(caa.id) == "CAA");
    CHECK(locked_caa.get_sdk_version() == "1, 7, 0, 0");
    locked_caa.close_rotator(caa.id);
    CHECK(caa.close_calls == 1);
}

// Falsified by: locked_zwo_eaf_sdk.h not forwarding a throw from the inner SDK (the scripted open
// failure is swallowed and the by-index driver reports connected).
TEST_CASE("FakeZWOEAFSDK behind LockedZWOEAFSDK - an inner throw reaches the driver and releases the lock",
          "[zwo][unit][fake-sdk]") {
    FakeZWOEAFSDK fake;
    fake.fail_open = true;
    LockedZWOEAFSDK sdk(fake);
    auto driver = zwo::create_zwo_eaf_focuser_by_index(0, 0, sdk);

    CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::DriverException);
    fake.fail_open = false;
    driver->set_connected(true);  // would deadlock if the failed call kept the decorator mutex
    CHECK(driver->get_connected());
}

// Falsified by: fake_zwo_efw_sdk.h set_position() dropping its range check (the out-of-range slot is
// stored and no InvalidValue is thrown).
TEST_CASE("FakeZWOEFWSDK - refuses an out-of-range slot and honours its scripted open delay",
          "[zwo][filterwheel][unit][fake-sdk]") {
    FakeZWOEFWSDK sdk;
    CHECK(error_code([&] { sdk.set_position(sdk.id, sdk.slot_count); }) == alpacacore::AlpacaError::InvalidValue);

    sdk.open_delay = std::chrono::milliseconds(60);
    const auto start = std::chrono::steady_clock::now();
    sdk.open_wheel(sdk.id);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(50));
}
