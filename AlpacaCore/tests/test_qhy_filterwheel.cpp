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
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>
#include <alpacacore/version.h>

#include <functional>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

using alpacacore::test::FakeQHYSDK;
using alpacacore::test::LockedQHYSDK;

// Connect coverage runs over the SDK seam (issue #321) -- the real SDK cannot
// initialise on a test runner, so nothing here reached set_connected(true)
// before the seam existed.
// One-camera fake, shared with the other QHY seam test files (issue #342):
// this was three verbatim copies, so a change to what a default test fake
// looks like had to be made in three places with nothing failing if it was
// made in two.
FakeQHYSDK make_fake(const std::string& id = "fake-qhy-0") { return FakeQHYSDK::with_one_camera(id); }

}  // namespace

TEST_CASE("QHY Filter Wheel Driver - Defaults", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::FilterWheel);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "QHY CFW");
}

TEST_CASE("QHY Filter Wheel Driver - Device metadata", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(3, 0);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY integrated color filter wheel driver");
    CHECK(driver->get_driver_info() == "AlpacaCore QHY Filter Wheel Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "QHY_CFW_3");
}

TEST_CASE("QHY Filter Wheel Driver - Not connected throws", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_position(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Filter Wheel Driver - Unsupported actions", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("QHY Filter Wheel Driver - Names and focus offsets configurable while disconnected",
          "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(1, 0);

    // Slot count is unknown until connect, so no length validation is imposed yet.
    REQUIRE(driver->get_focus_offsets().empty());
    REQUIRE(driver->get_names().empty());
    REQUIRE_NOTHROW(driver->set_focus_offsets({0, 10}));
    REQUIRE_NOTHROW(driver->set_names({"L", "R"}));
    REQUIRE(driver->get_focus_offsets() == std::vector<int>{0, 10});
    REQUIRE(driver->get_names() == std::vector<std::string>{"L", "R"});
}

TEST_CASE("QHY Filter Wheel Driver - Value range validation", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    // ASCOM precedence (per AGENTS.md): range validation precedes the
    // connection check. A negative position is unconditionally invalid, so it
    // is InvalidValue even while disconnected. The upper bound depends on the
    // hardware-reported slot count (unknown until connect), so an
    // in-range-but-unverifiable index reports NotConnected instead;
    // ConformU covers the connected out-of-range InvalidValue path on
    // hardware (matches the ToupTek AFW test pattern).
    require_alpaca_error([&]() { driver->set_position(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_position(99); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Filter Wheel Driver - State machine contracts", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);

    // Platform 7 DeviceState: Position throws while disconnected and is
    // omitted, leaving just the TimeStamp; the non-compliant "Connected"
    // entry must not appear.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("QHY Filter Wheel Driver - Unsupported methods", "[qhy][filterwheel][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("QHY Filter Wheel Driver - Create by id and device number assignment", "[qhy][filterwheel][unit]") {
    auto d0 = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);
    auto d1 = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(1, 0);
    auto by_id = alpacacore::vendor::qhy::create_qhy_filterwheel(7, "qhy-cfw-test-id");

    CHECK(d0->get_device_number() == 0);
    CHECK(d1->get_device_number() == 1);
    CHECK(d0->get_unique_id() != d1->get_unique_id());

    CHECK(by_id->get_device_number() == 7);
    CHECK(by_id->get_unique_id() == "QHY_CFW_qhy-cfw-test-id");
    CHECK(by_id->get_connected() == false);
    CHECK(by_id->get_device_type() == alpacacore::DeviceType::FilterWheel);
}

// ── Connect path (over the SDK seam, issue #321) ────────────────────────────

TEST_CASE("QHY Filter Wheel Driver - Connects and reads its slot count", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    fake.params[alpacacore::vendor::qhy::control::CFWSLOTSNUM] = 7.0;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    // The wheel works on the raw opened handle; it must NOT trigger the
    // imaging chip's init sequence.
    CHECK(fake.init_calls == 0);
    CHECK(driver->get_names().size() == 7);
    CHECK(driver->get_names()[0] == "Filter 1");
    CHECK(driver->get_focus_offsets().size() == 7);
    CHECK(driver->get_name() == "FakeQHY600 CFW");

    driver->set_connected(false);
    CHECK(fake.physical_closes == 1);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Filter Wheel Driver - A camera with no CFW port is refused", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    fake.controls_available.erase(alpacacore::vendor::qhy::control::CFWPORT);
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // The open must be rolled back, or the shared handle is pinned forever.
    CHECK(fake.physical_opens == 1);
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
}

TEST_CASE("QHY Filter Wheel Driver - An invalid slot count is refused", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    fake.params[alpacacore::vendor::qhy::control::CFWSLOTSNUM] = 0.0;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::DriverException);
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
}

TEST_CASE("QHY Filter Wheel Driver - Connect seeds the position cache", "[qhy][filterwheel][unit]") {
    // The warm-up read exists so the first Position poll after Connect does not
    // pay the ~100-130ms hardware round trip (it blows ConformU's FAST target).
    auto fake = make_fake();
    fake.cfw_position_script = {2};
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);

    driver->set_connected(true);
    const int reads_after_connect = fake.call_count("get_cfw_position");
    CHECK(reads_after_connect == 1);
    CHECK(driver->get_position() == 2);
    // Served from cache -- no second SDK round trip.
    CHECK(fake.call_count("get_cfw_position") == reads_after_connect);
}

TEST_CASE("QHY Filter Wheel Driver - A move reports -1 in transit then settles", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);
    driver->set_connected(true);

    driver->set_position(3);
    CHECK(fake.last_cfw_target == 3);
    // A transit reading that is not the commanded slot is masked to -1 rather
    // than passed through -- NINA read a passing slot number as an arrival.
    //
    // The script MUST be assigned after set_position(): move_cfw() clears any
    // pending script (see "a move clears any pending script" in
    // test_qhy_fake_sdk.cpp), so hoisting this line above the move would wipe
    // it and leave the case asserting against a settled position instead of a
    // transit -- passing for the wrong reason.
    fake.cfw_position_script = {1, 2, 3};
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == 3);

    driver->set_connected(false);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Filter Wheel Driver - A slot above the protocol ceiling issues no move", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    fake.params[alpacacore::vendor::qhy::control::CFWSLOTSNUM] = 12.0;
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel(0, "fake-qhy-0", sdk);
    driver->set_connected(true);

    // The CFW wire protocol is a single ASCII digit. Throwing here (rather
    // than from move_cfw) keeps pending_target_ clear, so later Position polls
    // are not degraded to live reads for a move that never happened.
    require_alpaca_error([&]() { driver->set_position(10); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.call_count("move_cfw") == 0);
}

TEST_CASE("QHY Filter Wheel Driver - Shares one physical handle with the paired camera", "[qhy][filterwheel][unit]") {
    // The miniCam8M's wheel hangs off the camera's USB handle. Each driver has
    // its own connect lifecycle over ONE open_count -- this pairing is the
    // reason the wrapper reference-counts at all, and it has never had a test.
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto camera = alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
    auto wheel = alpacacore::vendor::qhy::create_qhy_filterwheel(1, "fake-qhy-0", sdk);

    camera->set_connected(true);
    wheel->set_connected(true);
    CHECK(fake.physical_opens == 1);  // shared, opened once
    CHECK(fake.ref_count("fake-qhy-0") == 2);

    // Either one disconnecting must leave the other's handle alive.
    wheel->set_connected(false);
    CHECK(fake.physical_closes == 0);
    CHECK(camera->get_connected());
    CHECK(camera->get_camera_x_size() == 64);

    camera->set_connected(false);
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Filter Wheel Driver - Connecting by index with no cameras detected fails", "[qhy][filterwheel][unit]") {
    // The empty-enumeration branch of resolve_camera_id_locked() -- unreachable
    // by the by-id factory, only exercisable through _by_index.
    FakeQHYSDK fake;  // no cameras added
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("QHY Filter Wheel Driver - Connecting by an out-of-range index fails and leaks nothing",
          "[qhy][filterwheel][unit]") {
    auto fake = make_fake();  // exactly one camera, index 0
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 3, sdk);

    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.physical_opens == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("QHY Filter Wheel Driver - Connecting by index resolves the id and connects", "[qhy][filterwheel][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0, sdk);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.physical_opens == 1);
    CHECK(fake.last_opened_id == "fake-qhy-0");

    driver->set_connected(false);
    CHECK(fake.physical_closes == 1);
}
