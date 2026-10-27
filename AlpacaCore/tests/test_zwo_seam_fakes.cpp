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
#include <alpacacore/vendor/zwo/zwo_slots.h>

#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

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

TEST_CASE("ZWO accessory enumeration - failed ID and property reads skip only their SDK slots",
          "[zwo][unit][enumeration]") {
    struct Info {
        int ID{};
        std::string name;
    };
    for (const std::string kind : {"EFW", "EAF", "CAA"}) {
        CAPTURE(kind);
        for (const int failed_slot : {0, 1, 2}) {
            CAPTURE(failed_slot);
            for (const bool fail_id : {true, false}) {
                CAPTURE(fail_id);
                std::vector<int> id_reads;
                std::vector<int> property_reads;
                std::vector<std::string> warnings;
                struct SinkGuard {
                    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
                    ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
                } guard;
                alpacacore::logging::set_log_sink([&](auto level, auto, auto message) {
                    if (level == alpacacore::logging::LogLevel::Warn) warnings.emplace_back(message);
                });
                const auto slots = zwo::read_zwo_accessory_slots<Info>(
                    3, kind,
                    [&](int slot, int* id) {
                        id_reads.push_back(slot);
                        *id = 17 + slot * 25;
                        return fail_id && slot == failed_slot ? 5 : 0;
                    },
                    [&](int id, Info* info) {
                        property_reads.push_back(id);
                        info->ID = id;
                        info->name = kind;
                        return !fail_id && id == 17 + failed_slot * 25 ? 5 : 0;
                    },
                    [&](int) { return kind + "_ERROR_REMOVED"; });
                REQUIRE(slots.size() == 2);
                CHECK(id_reads == std::vector<int>{0, 1, 2});
                std::vector<int> expected_properties;
                std::size_t found = 0;
                for (int slot = 0; slot < 3; ++slot) {
                    if (!fail_id || slot != failed_slot) expected_properties.push_back(17 + slot * 25);
                    if (slot == failed_slot) continue;
                    CHECK(slots[found].slot == slot);
                    CHECK(slots[found].info.ID == 17 + slot * 25);
                    CHECK(slots[found].info.name == kind);
                    ++found;
                }
                CHECK(property_reads == expected_properties);
                REQUIRE(warnings.size() == 1);
                CHECK(warnings[0] == "Skipping " + kind + " slot " + std::to_string(failed_slot) + ": " + kind +
                                         (fail_id ? "GetID: " : "GetProperty: ") + kind + "_ERROR_REMOVED");
            }
        }
    }
}

TEST_CASE("ZWO accessory enumeration - empty and unreadable buses return no placeholder devices",
          "[zwo][unit][enumeration]") {
    struct Info {
        int ID{};
    };
    for (const int count : {-1, 0, 3}) {
        CAPTURE(count);
        for (const bool fail_id : {true, false}) {
            CAPTURE(fail_id);
            int id_reads = 0;
            int property_reads = 0;
            const auto slots = zwo::read_zwo_accessory_slots<Info>(
                count, "EFW",
                [&](int slot, int* id) {
                    ++id_reads;
                    *id = 17 + slot;
                    return fail_id ? 5 : 0;
                },
                [&](int, Info*) {
                    ++property_reads;
                    return 5;
                },
                [](int) { return std::string("EFW_ERROR_REMOVED"); });
            CHECK(slots.empty());
            CHECK(id_reads == (count > 0 ? count : 0));
            CHECK(property_reads == (!fail_id && count > 0 ? count : 0));
        }
    }
}

TEST_CASE("ZWO EFW enumeration - a configured index selects the SDK slot, not list position",
          "[zwo][filterwheel][unit][fake-sdk][enumeration]") {
    FakeZWOEFWSDK sdk;
    sdk.id = 42;
    sdk.slot_index = 1;
    SECTION("the surviving higher slot connects") {
        auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 1, sdk);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(sdk.last_open_id == 42);
        CHECK(driver->get_names().size() == 7);
        driver->set_connected(false);
        CHECK(sdk.open_calls == sdk.close_calls);
    }
    SECTION("the missing slot never binds the survivor") {
        auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);
        CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK_FALSE(driver->get_connected());
        CHECK(sdk.open_calls == 0);
    }
    SECTION("explicit ID binding does not depend on readable slots") {
        auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
        driver->set_connected(true);
        CHECK(sdk.last_open_id == 42);
        zwo::ZWOEFWInfo info;
        REQUIRE(sdk.get_wheel_info_by_id(sdk.id, info));
        CHECK(info.slot_index == -1);
        REQUIRE(sdk.get_wheel_info_by_index(1, info));
        CHECK(info.slot_index == 1);
        CHECK_FALSE(sdk.get_wheel_info_by_index(0, info));
    }
}

TEST_CASE("ZWO EAF enumeration - a configured index selects the SDK slot, not list position",
          "[zwo][focuser][unit][fake-sdk][enumeration]") {
    FakeZWOEAFSDK sdk;
    sdk.id = 42;
    sdk.slot_index = 1;
    SECTION("the surviving higher slot connects") {
        auto driver = zwo::create_zwo_eaf_focuser_by_index(0, 1, sdk);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(sdk.last_open_id == 42);
        CHECK(driver->get_max_step() == sdk.max_step);
        driver->set_connected(false);
        CHECK(sdk.open_calls == sdk.close_calls);
    }
    SECTION("the missing slot never binds the survivor") {
        auto driver = zwo::create_zwo_eaf_focuser_by_index(0, 0, sdk);
        CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK_FALSE(driver->get_connected());
        CHECK(sdk.open_calls == 0);
    }
    SECTION("reconnect follows the bound serial when the SDK slot changes") {
        auto driver = zwo::create_zwo_eaf_focuser_by_index(0, 1, sdk);
        driver->set_connected(true);
        driver->set_connected(false);
        sdk.slot_index = 2;
        sdk.id = 17;
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(sdk.last_open_id == 17);
        driver->set_connected(false);
        sdk.serial = "another-unit";
        CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::NotConnected);
        CHECK_FALSE(driver->get_connected());
        CHECK(sdk.open_calls == sdk.close_calls);
    }
    SECTION("explicit ID binding does not depend on readable slots") {
        auto driver = zwo::create_zwo_eaf_focuser(0, sdk.id, sdk);
        driver->set_connected(true);
        CHECK(sdk.last_open_id == 42);
        zwo::ZWOEAFFocuserInfo info;
        REQUIRE(sdk.get_focuser_info_by_id(sdk.id, info));
        CHECK(info.slot_index == -1);
        REQUIRE(sdk.get_focuser_info_by_index(1, info));
        CHECK(info.slot_index == 1);
        CHECK_FALSE(sdk.get_focuser_info_by_index(0, info));
    }
}

TEST_CASE("ZWO CAA enumeration - a configured index selects the SDK slot, not list position",
          "[zwo][rotator][unit][fake-sdk][enumeration]") {
    FakeZWOCAASDK sdk;
    sdk.id = 42;
    sdk.slot_index = 1;
    SECTION("the surviving higher slot connects") {
        auto driver = zwo::create_zwo_caa_rotator_by_index(0, 1, sdk);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(sdk.last_open_id == 42);
        driver->set_connected(false);
        CHECK(sdk.open_calls == sdk.close_calls);
    }
    SECTION("the missing slot never binds the survivor") {
        auto driver = zwo::create_zwo_caa_rotator_by_index(0, 0, sdk);
        CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK_FALSE(driver->get_connected());
        CHECK(sdk.open_calls == 0);
    }
    SECTION("explicit ID binding does not depend on readable slots") {
        auto driver = zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
        driver->set_connected(true);
        CHECK(sdk.last_open_id == 42);
        zwo::ZWOCAARotatorInfo info;
        REQUIRE(sdk.get_rotator_info_by_id(sdk.id, info));
        CHECK(info.slot_index == -1);
        REQUIRE(sdk.get_rotator_info_by_index(1, info));
        CHECK(info.slot_index == 1);
        CHECK_FALSE(sdk.get_rotator_info_by_index(0, info));
    }
}

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
    const auto eaf_open = locked_eaf.open_focuser_link(eaf.id);
    CHECK(eaf.open_calls == 1);
    CHECK(eaf_open.serial == "EAF-SN-1");
    const auto eaf_shared = locked_eaf.open_focuser_candidate(eaf.id);
    REQUIRE(eaf_shared.has_value());
    CHECK(eaf_shared->link == eaf_open.link);
    // Per-device calls run under the link (issue #1500), re-entering the decorator mutex with_link holds.
    locked_eaf.with_link(eaf.id, eaf_open.link, [&] {
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
    });
    CHECK(locked_eaf.get_sdk_version() == "1, 7, 0, 0");
    locked_eaf.close_focuser_link(eaf.id, eaf_shared->link);
    CHECK(eaf.close_calls == 0);
    locked_eaf.close_focuser_link(eaf.id, eaf_open.link);
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

// Falsified by: locked_zwo_eaf_sdk.h with_link() forwarding without its mutex (decision 0015 rule 2):
// another thread's call then runs while the link body is still inside the decorator.
TEST_CASE("FakeZWOEAFSDK behind LockedZWOEAFSDK - with_link holds the decorator mutex across its body",
          "[zwo][unit][fake-sdk]") {
    FakeZWOEAFSDK eaf;
    LockedZWOEAFSDK locked_eaf(eaf);
    const auto opened = locked_eaf.open_focuser_link(eaf.id);
    std::future<std::string> other;
    locked_eaf.with_link(eaf.id, opened.link, [&] {
        CHECK(locked_eaf.get_position(eaf.id) == eaf.position);  // the body re-enters on its own thread
        other = std::async(std::launch::async, [&] { return locked_eaf.get_sdk_version(); });
        CHECK(other.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
    });
    CHECK(other.get() == "1, 7, 0, 0");
    locked_eaf.close_focuser_link(eaf.id, opened.link);
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

// --- Position served through util::TtlStatusCache (issue #1072) ---
// The cache TTL is 100 ms; kPastTtl clears it. FakeZWOEFWSDK::position_calls counts the SDK reads.

namespace {

constexpr auto kPastTtl = std::chrono::milliseconds(150);

}  // namespace

// Falsified by: zwo_filterwheel_driver.cpp get_position() calling the SDK on every read (position_calls is 2
// inside the TTL, not 1), or a cache that never expires (no second SDK read after the TTL).
TEST_CASE("ZWO EFW seam - two Position reads inside the TTL cost one SDK call",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.position = 3;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);

    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);
    CHECK(sdk.position_calls == 1);

    sdk.position = 1;  // changes behind the driver's back: the frame still stands
    CHECK(driver->get_position() == 3);
    std::this_thread::sleep_for(kPastTtl);
    CHECK(driver->get_position() == 1);
    CHECK(sdk.position_calls == 2);
    driver->set_connected(false);
}

// Falsified by: zwo_filterwheel_driver.cpp set_position() without status_cache_.invalidate() after the SDK
// move (the read after the move is served from the pre-move frame: position 3, one SDK read).
TEST_CASE("ZWO EFW seam - a Position read after set_position goes to the SDK",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.position = 3;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);

    driver->set_position(4);
    CHECK(driver->get_position() == 4);
    CHECK(sdk.position_calls == 2);
    driver->set_connected(false);
}

// Falsified by: zwo_filterwheel_driver.cpp connect or disconnect without status_cache_.reset() (the first read
// after the reconnect is served from the previous session's frame: position 3, one SDK read).
TEST_CASE("ZWO EFW seam - a reconnect drops the cached Position",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.position = 3;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);

    driver->set_connected(false);
    sdk.position = 2;
    driver->set_connected(true);
    CHECK(driver->get_position() == 2);
    CHECK(sdk.position_calls == 2);
    driver->set_connected(false);
}

// Falsified by: zwo_filterwheel_driver.cpp reading Position without the cache's fault threshold (the third
// failed read throws the raw "link down", not "ZWO EFW communications compromised"), or serving the last good
// frame after a failed refill (the first failed read returns 3).
TEST_CASE("ZWO EFW seam - failed Position reads are not served and latch a link fault",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.position = 3;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 3);
    std::this_thread::sleep_for(kPastTtl);

    sdk.fail_reads = true;
    for (int i = 0; i < 2; ++i) {  // below the threshold the raw failure comes through, not the latch
        try {
            driver->get_position();
            FAIL("a failed read must throw");
        } catch (const AlpacaException& ex) {
            CHECK(std::string(ex.what()).find("communications compromised") == std::string::npos);
        }
    }
    try {
        driver->get_position();
        FAIL("third consecutive failure must throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }

    // A good refill clears the latch with no reconnect, and its frame is served again.
    sdk.fail_reads = false;
    const int before = sdk.position_calls;
    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);
    CHECK(sdk.position_calls == before + 1);
    driver->set_connected(false);
}

// Falsified by: zwo_filterwheel_driver.cpp caching a -1 frame (the second poll is served -1 from the cache: one
// SDK read for two polls), or mapping -1 to a slot (Position reads 0, not the ASCOM moving sentinel).
TEST_CASE("ZWO EFW seam - Position reads -1 while moving and the move end is seen at once",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.moving_reads = 2;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    driver->set_position(4);
    const int before = sdk.position_calls;

    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == -1);
    CHECK(driver->get_position() == 4);  // no sleep: a moving frame is never held
    CHECK(sdk.position_calls == before + 3);
    driver->set_connected(false);
}

// Decision 0016: a member that needs no device access never waits behind a Position refill.
// Falsified by: zwo_filterwheel_driver.cpp get_position() holding mutex_ across the SDK read (get_names() waits
// the whole 400 ms delay).
TEST_CASE("ZWO EFW seam - get_names does not wait behind a slow Position refill",
          "[zwo][filterwheel][unit][fake-sdk][position-cache][timing]") {
    FakeZWOEFWSDK sdk;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    sdk.position_delay = std::chrono::milliseconds(400);
    auto refill = std::async(std::launch::async, [&] { return driver->get_position(); });
    for (int i = 0; i < 500 && sdk.position_entered.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(sdk.position_entered.load() == 1);

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver->get_names().size() == 7);
    CHECK(driver->get_name() == "EFW");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
    refill.get();
    driver->set_connected(false);
}

// A move command that throws leaves the cached frame untrusted.
// Falsified by: zwo_filterwheel_driver.cpp set_position() skipping status_cache_.invalidate() when the SDK move
// throws (the read after the failed move is served from the old frame: no second SDK read).
TEST_CASE("ZWO EFW seam - a failed set_position invalidates the cached Position",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.position = 3;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    driver->set_connected(true);
    CHECK(driver->get_position() == 3);
    CHECK(driver->get_position() == 3);
    REQUIRE(sdk.position_calls == 1);

    sdk.fail_set_position = true;
    CHECK_THROWS_AS(driver->set_position(4), AlpacaException);
    sdk.fail_set_position = false;
    sdk.position = 2;
    CHECK(driver->get_position() == 2);
    CHECK(sdk.position_calls == 2);
    driver->set_connected(false);
}

// Decision 0016 rules 1 and 4: the connect handshake runs off the state lock.
// Falsified by: zwo_filterwheel_driver.cpp set_connected(true) holding mutex_ across open_wheel (get_names() and
// get_name() wait the whole 400 ms open delay).
TEST_CASE("ZWO EFW seam - get_names does not wait behind a held connect",
          "[zwo][filterwheel][unit][fake-sdk][position-cache][timing]") {
    FakeZWOEFWSDK sdk;
    auto driver = zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk);
    sdk.open_delay = std::chrono::milliseconds(400);
    auto connect = std::async(std::launch::async, [&] { driver->set_connected(true); });
    for (int i = 0; i < 500 && sdk.open_entered.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(sdk.open_entered.load() == 1);

    const auto start = std::chrono::steady_clock::now();
    // The names exist only once the connect has run; asserting no-throw and timing keeps a correct fix green.
    CHECK_NOTHROW(driver->get_names());
    CHECK_NOTHROW(driver->get_name());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
    connect.get();
    driver->set_connected(false);
}

// Decision 0016 rule 3: identity and limits publish at the end of connect, so a connect that throws after the
// enumeration leaves the pre-connect values.
// Falsified by: zwo_filterwheel_driver.cpp resolve_wheel_id() storing wheel_info_/wheel_info_valid_ again after the
// enumeration (get_name() reports the enumerated "EFW" and set_names() with 3 names is rejected for the 7-slot count).
TEST_CASE("ZWO EFW seam - a failed connect leaves the pre-connect name and slot limits",
          "[zwo][filterwheel][unit][fake-sdk][position-cache]") {
    FakeZWOEFWSDK sdk;
    sdk.fail_open = true;
    auto driver = zwo::create_zwo_efw_filterwheel_by_index(0, 0, sdk);
    REQUIRE_THROWS_AS(driver->set_connected(true), AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "ZWO EFW");
    CHECK_NOTHROW(driver->set_names({"A", "B", "C"}));
    CHECK(driver->get_names().size() == 3);
}
