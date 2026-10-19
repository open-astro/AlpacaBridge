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

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

#include "catch2_compat.h"
#include "fake_zwo_efw_sdk.h"
#include "locked_zwo_efw_sdk.h"

// The EFW SDK seam (issue #1309): the driver runs over a scripted fake, directly and behind the
// Locked decorator that serialises every SDK call.

namespace {

namespace zwo = alpacacore::vendor::zwo;
using alpacacore::AlpacaException;
using alpacacore::test::FakeZWOEFWSDK;
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
// 5, not 3). Every one of the 14 forwards has an assertion that fails when it is broken.
TEST_CASE("FakeZWOEFWSDK behind LockedZWOEFWSDK - forwards every call", "[zwo][unit][fake-sdk]") {
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
