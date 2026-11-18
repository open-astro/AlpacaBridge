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

// The OnStepX rotator (#2221) on the controller's shared link: the telescope and the rotator over one loopback
// FakeMountServer (the Network transport, onstep_controller.h). Replies follow OnStepX Rotator.command.cpp at af1d26bf
// (.github/instructions/onstep.instructions.md). The [stress] cases are in test_onstep_concurrency_stress.cpp.

#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/unique_id.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_rotator_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "rotator_sync_offset_test_dir.h"
#ifndef _WIN32
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "onstep_controller.h"
#endif

namespace onstep = alpacacore::vendor::onstep;
namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::DeviceType;
using alpacacore::test::require_alpaca_error;

namespace {

std::unique_ptr<alpacacore::RotatorDriver> make_offline_rotator(int device_number) {
    onstep::ConnectionInfo conn;
    conn.type = onstep::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    return onstep::create_onstep_rotator(device_number, conn);
}

}  // namespace

TEST_CASE("OnStep Rotator Driver - Defaults", "[onstep][rotator][unit]") {
    auto driver = make_offline_rotator(0);
    REQUIRE(driver->get_device_type() == DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_name() == "OnStep Rotator");
    CHECK(driver->get_can_reverse());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("OnStep Rotator Driver - Device metadata", "[onstep][rotator][unit]") {
    auto driver = make_offline_rotator(3);
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "OnStepX rotator (LX200 protocol)");
    CHECK(driver->get_driver_info() == "AlpacaCore OnStep Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "OnStep_Rotator_3");
}

TEST_CASE("OnStep Rotator Driver - Unsupported members", "[onstep][rotator][unit]") {
    auto driver = make_offline_rotator(0);
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    require_alpaca_error([&] { (void)driver->action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind(":rG#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_bool(":rG#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string(":rG#", false); }, AlpacaError::MethodNotImplemented);
    // Disconnected: every operational member is NotConnected.
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_mechanical_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_reverse(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_target_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move_absolute(90.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->halt(); }, AlpacaError::NotConnected);
}

// A position that is not a finite number is InvalidValue on every move and Sync, before the connection check.
// Falsified by: onstep_rotator_driver.cpp validate_angle() accepting NaN.
TEST_CASE("OnStep Rotator Driver - a non-finite position is InvalidValue while disconnected",
          "[onstep][rotator][unit]") {
    auto driver = make_offline_rotator(0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf}) {
        CAPTURE(bad);
        require_alpaca_error([&] { driver->move(bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->move_absolute(bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->move_mechanical(bad); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->sync(bad); }, AlpacaError::InvalidValue);
    }
}

// The move target is the angle inside the travel that reaches the requested one, nearest where the rotator is.
// Falsified by: onstep_rotator_driver.cpp rotator_angle_within_travel() returning `mechanical` without trying the
// turns k (no -10 for 350 in -180..180).
TEST_CASE("OnStep Rotator Driver - a mechanical angle maps into the firmware's travel", "[onstep][rotator][unit]") {
    using onstep::detail::rotator_angle_within_travel;
    CHECK(rotator_angle_within_travel(90.0, 0.0, -180.0, 180.0) == std::optional<double>(90.0));
    CHECK(rotator_angle_within_travel(350.0, 0.0, -180.0, 180.0) == std::optional<double>(-10.0));
    CHECK(rotator_angle_within_travel(180.0, 0.0, -180.0, 180.0).has_value());
    CHECK(rotator_angle_within_travel(0.0, 0.0, 0.0, 360.0) == std::optional<double>(0.0));
    // A travel of more than a turn holds two angles: the nearer one is taken.
    CHECK(rotator_angle_within_travel(10.0, 340.0, 0.0, 400.0) == std::optional<double>(370.0));
    CHECK(rotator_angle_within_travel(10.0, 20.0, 0.0, 400.0) == std::optional<double>(10.0));
    // Outside a travel of less than a turn: no angle.
    CHECK_FALSE(rotator_angle_within_travel(200.0, 90.0, 0.0, 180.0).has_value());
    CHECK_FALSE(rotator_angle_within_travel(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 180.0).has_value());
}

#ifndef _WIN32

namespace {

using alpacacore::test::onstep_controller::Controller;
using alpacacore::test::onstep_controller::ge_pairs_after;
using alpacacore::test::onstep_controller::run_with_a_sibling_at_every_exchange;

std::string connect_error(alpacacore::AlpacaDriver& driver) {
    try {
        driver.set_connected(true);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

void set_rotator(Controller& controller, double angle) {
    std::lock_guard<std::mutex> lock(controller.state->mutex);
    controller.state->rotator_angle = angle;
    controller.state->rotator_target = angle;
}

int count_moves(Controller& controller) { return controller.state->count(":rS"); }

}  // namespace

// MechanicalPosition is :rG# wrapped into 0-360, Position equals it with no Sync, StepSize is :rD#, and every getter
// answers from the frame the connect seeded. Falsified by: onstep_rotator_driver.cpp mechanical_of_locked() not
// wrapping the signed firmware angle (MechanicalPosition -30).
TEST_CASE("OnStep Rotator Driver - Device specifics once connected", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, -30.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_mechanical_position() == Catch::Approx(330.0));
    CHECK(driver->get_position() == Catch::Approx(330.0));
    CHECK(driver->get_step_size() == Catch::Approx(0.005));
    CHECK(driver->get_can_reverse());
    CHECK_FALSE(driver->get_reverse());
    CHECK_FALSE(driver->get_is_moving());
    CHECK(driver->get_target_position() == 0.0);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("On-Step 10.23a"));
    const auto state = driver->get_device_state();
    const auto has = [&](const char* name) {
        return std::any_of(state.begin(), state.end(), [&](const auto& e) { return e.name == name; });
    };
    CHECK(has("IsMoving"));
    CHECK(has("MechanicalPosition"));
    CHECK(has("Position"));
    CHECK(has("TimeStamp"));
    driver->set_connected(false);
}

// A step size the firmware does not report is PropertyNotImplemented, not a made-up value.
// Falsified by: onstep_rotator_driver.cpp check_controller() storing 0 when :rD# did not parse.
TEST_CASE("OnStep Rotator Driver - no step size is PropertyNotImplemented", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->rotator_step = "?";
    }
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::PropertyNotImplemented);
    driver->set_connected(false);
}

// MoveMechanical sends :rS with the angle inside the travel (350 is -10 in -180..180), MoveAbsolute and Move apply the
// Sync offset first, and TargetPosition is the move's Position. Falsified by: onstep_rotator_driver.cpp
// move_absolute() sending `position` without subtracting sync_offset_.
TEST_CASE("OnStep Rotator Driver - moves go to the angle inside the travel", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, 0.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*driver, true);

    driver->move_mechanical(350.0);
    CHECK(controller.state->count(":rS-010*00:00#") == 1);
    CHECK(driver->get_target_position() == Catch::Approx(350.0));
    driver->halt();
    set_rotator(controller, 0.0);
    onstep::poll_onstep_rotator_now_for_testing(*driver);

    driver->sync(100.0);  // Position 100 at mechanical 0: the offset is 100
    CHECK(driver->get_position() == Catch::Approx(100.0));
    driver->move_absolute(130.0);  // mechanical 30
    CHECK(controller.state->count(":rS+030*00:00#") == 1);
    CHECK(driver->get_target_position() == Catch::Approx(130.0));
    driver->halt();
    set_rotator(controller, 30.0);
    onstep::poll_onstep_rotator_now_for_testing(*driver);

    driver->move(-45.0);  // relative to Position 130: Position 85, mechanical -15
    CHECK(controller.state->count(":rS-015*00:00#") == 1);
    CHECK(driver->get_target_position() == Catch::Approx(85.0));
    driver->move_absolute(405.0);  // wraps to 45: mechanical -55
    CHECK(controller.state->count(":rS-055*00:00#") == 1);
    driver->set_connected(false);
}

// A target the travel cannot reach is InvalidValue, and nothing is sent.
// Falsified by: onstep_rotator_driver.cpp move_mechanical_to() sending :rS when rotator_angle_within_travel()
// found no angle.
TEST_CASE("OnStep Rotator Driver - a target outside the travel is InvalidValue with nothing sent",
          "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->rotator_min = 0;
        controller.state->rotator_max = 180;
    }
    set_rotator(controller, 90.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { driver->move_mechanical(200.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->move_absolute(270.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->move(120.0); }, AlpacaError::InvalidValue);
    CHECK(count_moves(controller) == 0);
    driver->move_mechanical(180.0);  // the limit itself is inside
    CHECK(controller.state->count(":rS+180*00:00#") == 1);
    driver->set_connected(false);
}

// A move writes IsMoving through, the poll follows :rT# to the end, and Halt sends :rQ# and writes not-moving
// through. Falsified by: onstep_rotator_driver.cpp move_mechanical_to() without its state_.write() (IsMoving reads
// false until the next poll).
TEST_CASE("OnStep Rotator Driver - State machine", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, 0.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*driver, true);
    CHECK_FALSE(driver->get_is_moving());

    driver->move_mechanical(30.0);  // two status reads of 20 degrees, then stopped
    CHECK(driver->get_is_moving());
    onstep::poll_onstep_rotator_now_for_testing(*driver);
    CHECK(driver->get_is_moving());
    CHECK(driver->get_mechanical_position() == Catch::Approx(20.0));
    onstep::poll_onstep_rotator_now_for_testing(*driver);
    CHECK_FALSE(driver->get_is_moving());
    CHECK(driver->get_mechanical_position() == Catch::Approx(30.0));

    driver->move_mechanical(150.0);
    REQUIRE(driver->get_is_moving());
    driver->halt();
    CHECK(controller.state->count(":rQ#") == 1);
    CHECK_FALSE(driver->get_is_moving());
    onstep::poll_onstep_rotator_now_for_testing(*driver);
    CHECK_FALSE(driver->get_is_moving());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
}

// A move while the firmware's derotator runs (:rT# 'D') is InvalidOperation, read on the wire right before the move,
// and no :rS goes out. Falsified by: onstep_rotator_driver.cpp move_mechanical_to() without its derotating check.
TEST_CASE("OnStep Rotator Driver - a move while derotating is InvalidOperation", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*driver, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->derotating = true;  // switched on at the controller after the last poll
    }
    require_alpaca_error([&] { driver->move_absolute(90.0); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->move_mechanical(90.0); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->move(5.0); }, AlpacaError::InvalidOperation);
    CHECK(count_moves(controller) == 0);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->derotating = false;
    }
    driver->move_absolute(90.0);
    CHECK(count_moves(controller) == 1);
    driver->set_connected(false);
}

// A move the controller refuses ('0') throws InvalidOperation with the :GE# reason, read right after it.
// Falsified by: onstep_rotator_driver.cpp move_mechanical_to() ignoring rotator_goto()'s false.
TEST_CASE("OnStep Rotator Driver - a refused move reports the controller's reason", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->rotator_goto_reply = "0";
        controller.state->ge_code = "09#";  // CE_PARKED (Rotator.cpp gotoTarget())
    }
    std::string message;
    try {
        driver->move_absolute(60.0);
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidOperation);
        message = e.what();
    }
    CHECK(message.find("the rotator is parked") != std::string::npos);
    CHECK_FALSE(driver->get_is_moving());
    const auto log = controller.state->commands();
    const auto goto_at = std::find(log.begin(), log.end(), ":rS+060*00:00#");
    REQUIRE(goto_at != log.end());
    REQUIRE(goto_at + 1 != log.end());
    CHECK(*(goto_at + 1) == ":GE#");
    driver->set_connected(false);
}

// Sync sets the offset from the live :rG#, saves it under the stored UniqueID (decision 0008), and a later connect,
// of this driver object or a new one, loads it. Falsified by: onstep_rotator_driver.cpp open_locked() not loading
// RotatorSyncOffsetStore (Position back to MechanicalPosition after the reconnect).
TEST_CASE("OnStep Rotator Driver - Sync persists the offset across a reconnect",
          "[onstep][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, 40.0);
    const std::string guid = alpacacore::util::generate_unique_id();
    auto driver = controller.rotator();
    driver->assign_unique_id(guid);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->sync(10.0);
    CHECK(driver->get_position() == Catch::Approx(10.0));
    CHECK(driver->get_mechanical_position() == Catch::Approx(40.0));
    CHECK(alpacacore::util::RotatorSyncOffsetStore::load(guid) == Catch::Approx(330.0));
    driver->set_connected(false);

    driver->set_connected(true);
    CHECK(driver->get_position() == Catch::Approx(10.0));
    driver->set_connected(false);

    auto rebuilt = controller.rotator();
    rebuilt->assign_unique_id(guid);
    rebuilt->set_connected(true);
    CHECK(rebuilt->get_position() == Catch::Approx(10.0));
    rebuilt->set_connected(false);
}

// Decision 0008: a Sync that cannot store its offset fails and the previous offset stays.
// Falsified by: onstep_rotator_driver.cpp sync() applying the offset when RotatorSyncOffsetStore::save() fails.
TEST_CASE("OnStep Rotator Driver - Sync that cannot save throws and keeps the offset",
          "[onstep][rotator][sync-offset][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, 0.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->sync(30.0);
    REQUIRE(driver->get_position() == Catch::Approx(30.0));

    // A regular file where the store's directory should be: every save fails.
    const auto blocker = dir.file().parent_path().parent_path() / "blocker";
    { std::ofstream(blocker) << "x"; }
    alpacacore::util::RotatorSyncOffsetStore::set_default_path((blocker / "offsets.json").string());
    require_alpaca_error([&] { driver->sync(50.0); }, AlpacaError::DriverException);
    CHECK(driver->get_position() == Catch::Approx(30.0));
    driver->set_connected(false);
}

// IRotatorV4: CanReverse is true and Reverse is implemented. The firmware's :rR# reverses only its derotator, so
// Reverse is the driver's: it mirrors MechanicalPosition and the angle each move sends, and nothing reaches the wire.
// Falsified by: onstep_rotator_driver.cpp move_mechanical_to() sending `mechanical` without mirroring it while
// reverse_ is set.
TEST_CASE("OnStep Rotator Driver - Reverse mirrors the angle and sends nothing", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    set_rotator(controller, 30.0);
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*driver, true);
    const auto before = controller.state->commands().size();
    driver->set_reverse(true);
    CHECK(controller.state->commands().size() == before);
    CHECK(driver->get_reverse());
    CHECK(driver->get_mechanical_position() == Catch::Approx(330.0));
    CHECK(driver->get_position() == Catch::Approx(330.0));  // no Sync: Position equals MechanicalPosition
    driver->move_mechanical(300.0);                         // firmware angle 60
    CHECK(controller.state->count(":rS+060*00:00#") == 1);
    driver->halt();
    driver->set_reverse(false);
    CHECK(driver->get_mechanical_position() == Catch::Approx(30.0));
    driver->set_connected(false);
}

// The derotator is the firmware's: no member ever enables, disables, aims or reverses it.
// Falsified by: onstep_protocol_wrapper.cpp rotator_halt() sending ":r-#" before ":rQ#".
TEST_CASE("OnStep Rotator Driver - never sends a derotator command", "[onstep][rotator][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto driver = controller.rotator();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::poll_onstep_rotator_now_for_testing(*driver);
    driver->move_absolute(60.0);
    driver->halt();
    driver->set_reverse(true);
    driver->move(10.0);
    driver->halt();
    driver->sync(0.0);
    driver->set_connected(false);
    for (const auto& c : controller.state->commands()) {
        CAPTURE(c);
        CHECK(c != ":r+#");
        CHECK(c != ":r-#");
        CHECK(c != ":rP#");
        CHECK(c != ":rR#");
    }
}

// Connect refuses OnStep 4.x and a controller without the rotator, naming why, and holds no link after.
// Falsified by: onstep_rotator_driver.cpp check_controller() without its rotator_capability() check.
TEST_CASE("OnStep Rotator Driver - Connect refuses OnStep 4.x and a controller without a rotator",
          "[onstep][rotator][unit]") {
    Controller controller;
    SECTION("OnStep 4.x") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->version = "4.24s";
        }
        auto driver = controller.rotator();
        CHECK(connect_error(*driver).find("requires OnStepX firmware") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
        CHECK(controller.state->count(":r") == 0);  // nothing sent to a firmware the driver has not checked
    }
    SECTION("the firmware reports no rotator (N)") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->rotator_kind = "N";
        }
        auto driver = controller.rotator();
        CHECK(connect_error(*driver).find("OnStepX reports no rotator") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
    }
    SECTION("the firmware does not know :GX98#") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->rotator_kind = "0";
        }
        auto driver = controller.rotator();
        CHECK(connect_error(*driver).find("OnStepX reports no rotator") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
    }
    SECTION("a derotator (D) connects") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->rotator_kind = "D";
        }
        alpacacore::test::TempSyncOffsetDir dir;
        auto driver = controller.rotator();
        driver->set_connected(true);
        CHECK(driver->get_connected());
        driver->set_connected(false);
    }
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);  // the link closed again
}

// Decision 0010: DeviceState's TimeStamp is when the served frame was measured. Falsified by:
// onstep_rotator_driver.cpp device_state_age() delete the override (the TimeStamp is the request time).
TEST_CASE("OnStep Rotator Driver - DeviceState TimeStamp is when the frame was measured",
          "[onstep][rotator][snapshot][unit]") {
    alpacacore::test::TempSyncOffsetDir dir;
    alpacacore::test::FakeTaskClock clock;
    Controller controller;
    auto driver = onstep::create_onstep_rotator(0, controller.info(), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*driver, true);
    onstep::poll_onstep_rotator_now_for_testing(*driver);  // a frame measured at the fake clock's now
    clock.advance(std::chrono::milliseconds(900));         // paused: no poll, the frame ages

    std::string stamp;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "TimeStamp") stamp = std::get<std::string>(entry.value);
    }
    const auto taken = std::chrono::system_clock::now();
    REQUIRE_FALSE(stamp.empty());
    const auto stamped = alpacacore::test::parse_timestamp(stamp);
    const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(taken - stamped);
    INFO("TimeStamp is " << behind.count() << " ms behind now");
    CHECK(behind >= std::chrono::milliseconds(800));  // the 900 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));
    driver->set_connected(false);
}

// A member whose connection check passed just before a Disconnect answers NotConnected, never a travel, step size or
// Position computed from the connect-time values the Disconnect cleared (the #2387 TSan finding, sibling of the
// switch). Falsified by: onstep_rotator_driver.cpp require_connect_state_locked() never throwing
// (PropertyNotImplemented).
TEST_CASE("OnStep Rotator Driver - a member racing a Disconnect answers NotConnected", "[onstep][rotator][unit]") {
    using alpacacore::test::onstep_controller::code_with_disconnect_after_check;
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto driver = controller.rotator();
    const std::vector<std::pair<const char*, std::function<void()>>> members{
        {"StepSize", [&] { (void)driver->get_step_size(); }},
        {"TargetPosition", [&] { (void)driver->get_target_position(); }},
        {"MoveMechanical", [&] { driver->move_mechanical(90.0); }},
        {"MoveAbsolute", [&] { driver->move_absolute(90.0); }},
    };
    for (const auto& [name, member] : members) {
        CAPTURE(name);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(code_with_disconnect_after_check(*driver, member) == AlpacaError::NotConnected);
        CHECK_FALSE(driver->get_connected());
    }
}

// ── The shared link (#2221) ──────────────────────────────────────────────────────────────────────────────────────

// One link per controller, whichever device connects first; each keeps its own Connected when the other goes.
// Falsified by: onstep_rotator_driver.cpp release_link() calling link->disconnect() before letting go (the telescope
// loses the link with the rotator's Disconnect).
TEST_CASE("OnStep shared link - telescope then rotator share one link", "[onstep][rotator][shared-link]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto telescope = controller.telescope();
    auto rotator = controller.rotator();
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    rotator->set_connected(true);
    REQUIRE(rotator->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    CHECK(rotator->get_mechanical_position() == Catch::Approx(30.0));

    // The rotator goes; the telescope keeps the link and its poll.
    rotator->set_connected(false);
    CHECK_FALSE(rotator->get_connected());
    CHECK(telescope->get_connected());
    const int polls = controller.state->count(":GU#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":GU#") >= polls + 1; }, 3000));
    CHECK_NOTHROW((void)telescope->get_right_ascension());

    // The rotator joins again, still on the one link.
    rotator->set_connected(true);
    CHECK(rotator->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    telescope->set_connected(false);
    rotator->set_connected(false);
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// Falsified by: onstep_rotator_driver.cpp open_locked() opening its own link (OnStepProtocolWrapper{} and connect())
// instead of acquire().
TEST_CASE("OnStep shared link - rotator then telescope share one link", "[onstep][rotator][shared-link]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto telescope = controller.telescope();
    auto rotator = controller.rotator();
    rotator->set_connected(true);
    REQUIRE(rotator->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 1);

    // The telescope goes; the rotator keeps the link, and its poll keeps reaching the controller.
    telescope->set_connected(false);
    CHECK_FALSE(telescope->get_connected());
    CHECK(rotator->get_connected());
    const int polls = controller.state->count(":rT#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":rT#") >= polls + 2; }, 3000));
    rotator->move_absolute(60.0);
    CHECK(controller.state->count(":rS+060*00:00#") == 1);
    CHECK(controller.server.accepted_connections() == 1);
    rotator->set_connected(false);
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// A hangup is a lost link for every device on it (decision 0009 rule 2a): both read Connected false with no client
// I/O, both reconnect on their own Connect, and the reconnect is one link again.
// Falsified by: onstep_rotator_driver.cpp link_up() reading only the poll's loss latch, not the link's link_alive().
TEST_CASE("OnStep shared link - a link loss reaches the telescope and the rotator", "[onstep][rotator][shared-link]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto telescope = controller.telescope();
    auto rotator = controller.rotator();
    telescope->set_connected(true);
    rotator->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(rotator->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*rotator, true);

    REQUIRE(controller.server.drop_connections());
    CHECK(alpacacore::test::wait_until([&] { return !telescope->get_connected(); }, 2000));
    CHECK_FALSE(rotator->get_connected());
    CHECK(rotator->get_link_fault().find("lost") != std::string::npos);
    require_alpaca_error([&] { (void)rotator->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { rotator->move_absolute(10.0); }, AlpacaError::NotConnected);

    rotator->set_connected(true);
    REQUIRE(rotator->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 2);  // the first link, then one fresh one for both
    telescope->set_connected(false);
    rotator->set_connected(false);
}

// A refused rotator move (:rS answered 0) and its :GE# stay together on the shared link, and so does the :rT# that
// cleared it. Falsified by: onstep_rotator_driver.cpp move_mechanical_to() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a rotator move and its :GE#",
          "[onstep][rotator][shared-link]") {
    alpacacore::test::TempSyncOffsetDir dir;
    Controller controller;
    auto telescope = controller.telescope();
    auto rotator = controller.rotator();
    telescope->set_connected(true);
    rotator->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(rotator->get_connected());
    onstep::set_onstep_rotator_poll_paused_for_testing(*rotator, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->rotator_goto_reply = "0#";  // a '#' ends the bare digit at once
        controller.state->ge_code = "09#";
    }
    constexpr int kMoves = 10;
    int refused = 0;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kMoves, [&] {
        try {
            rotator->move_absolute(60.0);
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation) ++refused;
        }
    });
    CHECK(refused == kMoves);
    CHECK(ge_pairs_after(log, ":rS") == kMoves);
    int checked = 0;
    for (std::size_t i = 1; i < log.size(); ++i) {
        if (log[i].rfind(":rS", 0) == 0) {
            ++checked;
            CAPTURE(i);
            CHECK(log[i - 1] == ":rT#");  // the derotation check right before, with nothing between
        }
    }
    CHECK(checked == kMoves);
    telescope->set_connected(false);
    rotator->set_connected(false);
}

#endif  // !_WIN32
