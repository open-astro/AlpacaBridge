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

// The OnStepX focuser (#2223) and the controller's shared link: the telescope and the focuser over one loopback
// FakeMountServer (the Network transport, onstep_controller.h), so every case also shows how many links the
// controller saw. Replies follow OnStepX Focuser.command.cpp at af1d26bf (.github/instructions/onstep.instructions.md).
// The [stress] cases are in test_onstep_concurrency_stress.cpp.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/onstep/onstep_focuser_driver.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
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

std::unique_ptr<alpacacore::FocuserDriver> make_offline_focuser(int device_number, int focuser_number = 1) {
    onstep::ConnectionInfo conn;
    conn.type = onstep::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    return onstep::create_onstep_focuser(device_number, conn, focuser_number);
}

}  // namespace

TEST_CASE("OnStep Focuser Driver - Defaults", "[onstep][focuser][unit]") {
    auto driver = make_offline_focuser(0);
    REQUIRE(driver->get_device_type() == DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_name() == "OnStep Focuser");
    CHECK(driver->get_absolute());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("OnStep Focuser Driver - Device metadata", "[onstep][focuser][unit]") {
    auto driver = make_offline_focuser(3, 2);
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "OnStepX focuser 2 (LX200 protocol)");
    CHECK(driver->get_driver_info() == "AlpacaCore OnStep Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "OnStep_Focuser_3");
}

// The factory refuses a focuser number OnStepX cannot address (:F1...# to :F6...#), before anything is opened.
// Falsified by: onstep_focuser_driver.cpp checked_focuser_number() accepting 0 or 7.
TEST_CASE("OnStep Focuser Driver - a focuser number outside 1-6 is InvalidValue", "[onstep][focuser][unit]") {
    for (const int n : {0, 7, -1}) {
        CAPTURE(n);
        require_alpaca_error([n] { (void)make_offline_focuser(0, n); }, AlpacaError::InvalidValue);
    }
    for (const int n : {1, 6}) {
        CHECK_NOTHROW((void)make_offline_focuser(0, n));
    }
}

TEST_CASE("OnStep Focuser Driver - Unsupported members", "[onstep][focuser][unit]") {
    auto driver = make_offline_focuser(0);
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    require_alpaca_error([&] { (void)driver->action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind(":GU#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_bool(":GU#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string(":GU#", false); }, AlpacaError::MethodNotImplemented);
    // Disconnected: every operational member is NotConnected, a Move outside the range included.
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_max_step(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move(-5); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->halt(); }, AlpacaError::NotConnected);
}

#ifndef _WIN32

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

using alpacacore::test::onstep_controller::Controller;
using alpacacore::test::onstep_controller::OnStepXController;

std::string connect_error(alpacacore::AlpacaDriver& driver) {
    try {
        driver.set_connected(true);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

// Position and MaxStep count from the minimum (:F<n>g# - :F<n>i#, :F<n>m# - :F<n>i#), StepSize is :F<n>u#,
// Temperature :F<n>t#, and every getter answers from the frame the connect seeded. Falsified by:
// onstep_focuser_driver.cpp probe() serving the raw :F<n>g# without subtracting min_position_.
TEST_CASE("OnStep Focuser Driver - Device specifics once connected", "[onstep][focuser][unit]") {
    Controller controller;
    auto driver = controller.focuser();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_max_step() == 50000);
    CHECK(driver->get_max_increment() == 50000);
    CHECK(driver->get_step_size() == Catch::Approx(0.5));
    CHECK(driver->get_temperature() == Catch::Approx(21.5));
    CHECK(driver->get_temp_comp_available());
    CHECK_FALSE(driver->get_temp_comp());
    CHECK(driver->get_absolute());
    CHECK_FALSE(driver->get_is_moving());
    CHECK(driver->get_device_firmware() == std::optional<std::string>("On-Step 10.23a"));
    const auto state = driver->get_device_state();
    const auto has = [&](const char* name) {
        return std::any_of(state.begin(), state.end(), [&](const auto& e) { return e.name == name; });
    };
    CHECK(has("IsMoving"));
    CHECK(has("Position"));
    CHECK(has("Temperature"));
    CHECK(has("TimeStamp"));
    driver->set_connected(false);
}

// Decision 0010: DeviceState's TimeStamp is when the served frame was measured. The poll is paused after one fresh
// frame and the fake clock moved 900 ms, so a request-time stamp is ~0 ms behind and the frame's is ~900 ms behind.
// Falsified by: onstep_focuser_driver.cpp device_state_age() delete the override (the TimeStamp is the request time).
TEST_CASE("OnStep Focuser Driver - DeviceState TimeStamp is when the frame was measured",
          "[onstep][focuser][snapshot][unit]") {
    alpacacore::test::FakeTaskClock clock;
    Controller controller;
    auto driver = onstep::create_onstep_focuser(0, controller.info(), 1, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*driver, true);
    onstep::poll_onstep_focuser_now_for_testing(*driver);  // a frame measured at the fake clock's now
    clock.advance(std::chrono::milliseconds(900));         // paused: no poll, the frame ages

    std::string stamp;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "TimeStamp") stamp = std::get<std::string>(entry.value);
    }
    const auto taken = std::chrono::system_clock::now();
    REQUIRE_FALSE(stamp.empty());
    std::tm tm{};
    int ms = 0;
    REQUIRE(std::sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                        &tm.tm_min, &tm.tm_sec, &ms) == 7);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    const auto stamped = std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
    const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(taken - stamped);
    INFO("TimeStamp is " << behind.count() << " ms behind now");
    CHECK(behind >= std::chrono::milliseconds(800));  // the 900 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));
    driver->set_connected(false);
}

// #2028: a Move outside 0..MaxStep goes to that limit on the wire, in raw steps (+ the minimum).
// Falsified by: onstep_focuser_driver.cpp move() sending the client's position without the clamp.
TEST_CASE("OnStep Focuser Driver - a move outside 0..MaxStep goes to that limit", "[onstep][focuser][unit]") {
    Controller controller;
    auto driver = controller.focuser();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->move(-500);
    CHECK(controller.state->count(":F1s100#") == 1);  // 0 + the minimum
    driver->move(99999);
    CHECK(controller.state->count(":F1s50100#") == 1);  // MaxStep + the minimum
    driver->move(2000);
    CHECK(controller.state->count(":F1s2100#") == 1);
    driver->set_connected(false);
}

// Move writes IsMoving through, the poll follows :F<n>T# to the end, and Halt sends :F<n>Q# and writes not-moving
// through. Falsified by: onstep_focuser_driver.cpp move() without its state_.write() (IsMoving reads false until the
// next poll), or halt() without its write-through.
TEST_CASE("OnStep Focuser Driver - State machine", "[onstep][focuser][unit]") {
    Controller controller;
    auto driver = controller.focuser();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*driver, true);
    CHECK_FALSE(driver->get_is_moving());

    driver->move(11000);  // raw 11100: two status reads of 5000 steps, then the last 0
    CHECK(driver->get_is_moving());
    onstep::poll_onstep_focuser_now_for_testing(*driver);
    CHECK(driver->get_is_moving());
    CHECK(driver->get_position() == 6000);
    onstep::poll_onstep_focuser_now_for_testing(*driver);
    CHECK_FALSE(driver->get_is_moving());
    CHECK(driver->get_position() == 11000);

    driver->move(40000);
    REQUIRE(driver->get_is_moving());
    driver->halt();
    CHECK(controller.state->count(":F1Q#") == 1);
    CHECK_FALSE(driver->get_is_moving());
    onstep::poll_onstep_focuser_now_for_testing(*driver);
    CHECK_FALSE(driver->get_is_moving());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
}

// A Move the controller refuses ('0') throws InvalidOperation with the :GE# reason, read right after it.
// Falsified by: onstep_focuser_driver.cpp move() ignoring focuser_goto()'s false.
TEST_CASE("OnStep Focuser Driver - a refused move reports the controller's reason", "[onstep][focuser][unit]") {
    Controller controller;
    auto driver = controller.focuser();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->goto_reply = "0";
        controller.state->ge_code = "09#";  // CE_PARKED: the focuser is parked (Focuser.cpp gotoTarget())
    }
    std::string message;
    try {
        driver->move(3000);
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidOperation);
        message = e.what();
    }
    CHECK(message.find("the focuser is parked") != std::string::npos);
    CHECK_FALSE(driver->get_is_moving());
    // The :GE# came right after the :F1s, with nothing between them.
    const auto log = controller.state->commands();
    const auto goto_at = std::find(log.begin(), log.end(), ":F1s3100#");
    REQUIRE(goto_at != log.end());
    REQUIRE(goto_at + 1 != log.end());
    CHECK(*(goto_at + 1) == ":GE#");
    driver->set_connected(false);
}

// TempComp reads :F<n>c# and writes :F<n>c0#/:F<n>c1#; with no temperature probe ("nan") OnStepX keeps it off
// (Focuser.cpp setTcfEnable()), so it is not available and Temperature is not implemented.
// Falsified by: onstep_focuser_driver.cpp check_controller() setting temp_comp_available_ from :F<n>c# alone.
TEST_CASE("OnStep Focuser Driver - temperature compensation and the temperature probe", "[onstep][focuser][unit]") {
    Controller controller;
    SECTION("with a probe: TempComp is read and written") {
        auto driver = controller.focuser();
        driver->set_connected(true);
        REQUIRE(driver->get_temp_comp_available());
        driver->set_temp_comp(true);
        CHECK(controller.state->count(":F1c1#") == 1);
        CHECK(driver->get_temp_comp());
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->tcf_reply = "0";
            controller.state->ge_code = "09#";
        }
        require_alpaca_error([&] { driver->set_temp_comp(false); }, AlpacaError::InvalidOperation);
        CHECK(driver->get_temp_comp());
        driver->set_connected(false);
    }
    SECTION("without a probe: not available, and Temperature not implemented") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->temperature = "nan";
        }
        auto driver = controller.focuser();
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK_FALSE(driver->get_temp_comp_available());
        CHECK_FALSE(driver->get_temp_comp());
        require_alpaca_error([&] { driver->set_temp_comp(true); }, AlpacaError::NotImplemented);
        CHECK(controller.state->count(":F1c1#") == 0);
        require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::PropertyNotImplemented);
        driver->set_connected(false);
    }
    SECTION("no step size: StepSize not implemented") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->microns = "?";
        }
        auto driver = controller.focuser();
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::PropertyNotImplemented);
        driver->set_connected(false);
    }
}

// Connect refuses OnStep 4.x and a focuser the controller does not have, naming why, and holds no link after.
// Falsified by: onstep_focuser_driver.cpp check_controller() without its is_onstepx_version() check.
TEST_CASE("OnStep Focuser Driver - Connect refuses OnStep 4.x and a focuser that is not there",
          "[onstep][focuser][unit]") {
    Controller controller;
    SECTION("OnStep 4.x") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->version = "4.24s";
        }
        auto driver = controller.focuser();
        CHECK(connect_error(*driver).find("requires OnStepX firmware") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
        CHECK(controller.state->count(":F1") == 0);  // nothing sent to a firmware the driver has not checked
    }
    SECTION("no focuser 2") {
        auto driver = controller.focuser(2);
        CHECK(connect_error(*driver).find("OnStepX reports no focuser 2") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
    }
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);  // the link closed again
}

// Only addressed :F<n>...# forms go out: never the active-focuser select :FA<n>#, never a bare :F<d># preset.
// Falsified by: onstep_protocol_wrapper.cpp focuser_command() building ":F" + body without the focuser digit.
TEST_CASE("OnStep Focuser Driver - only addressed focuser commands are sent", "[onstep][focuser][unit]") {
    Controller controller;
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->focusers = {3};
    }
    auto driver = controller.focuser(3);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::poll_onstep_focuser_now_for_testing(*driver);
    driver->move(500);
    driver->halt();
    driver->set_temp_comp(true);
    driver->set_connected(false);
    int focuser_commands = 0;
    for (const auto& c : controller.state->commands()) {
        if (c.rfind(":F", 0) != 0) continue;
        ++focuser_commands;
        CAPTURE(c);
        CHECK(c.size() > 4);
        CHECK(c[2] == '3');
        CHECK(c.rfind(":FA", 0) != 0);
    }
    CHECK(focuser_commands >= 8);
}

// ── The shared link (#2223) ──────────────────────────────────────────────────────────────────────────────────────

// One link per controller, whichever device connects first; each keeps its own Connected when the other goes.
// Falsified by: onstep_protocol_wrapper.cpp acquire() opening a fresh link instead of returning the live one (two
// TCP connections).
TEST_CASE("OnStep shared link - telescope then focuser share one link", "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    focuser->set_connected(true);
    REQUIRE(focuser->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    CHECK(focuser->get_position() == 1000);
    CHECK_NOTHROW((void)telescope->get_right_ascension());

    // The telescope goes; the focuser keeps the link, and its poll keeps reaching the controller.
    telescope->set_connected(false);
    CHECK_FALSE(telescope->get_connected());
    CHECK(focuser->get_connected());
    const int polls = controller.state->count(":F1T#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":F1T#") >= polls + 2; }, 3000));
    focuser->move(4000);
    CHECK(controller.state->count(":F1s4100#") == 1);
    CHECK(controller.server.accepted_connections() == 1);

    // The last holder closes the link.
    focuser->set_connected(false);
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// Falsified by: onstep_telescope_driver.cpp set_connected() opening its own link (OnStepProtocolWrapper{} and
// connect()) instead of acquire().
TEST_CASE("OnStep shared link - focuser then telescope share one link", "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    focuser->set_connected(true);
    REQUIRE(focuser->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 1);

    // The focuser goes; the telescope keeps the link and its own poll runs on.
    focuser->set_connected(false);
    CHECK_FALSE(focuser->get_connected());
    CHECK(telescope->get_connected());
    const int polls = controller.state->count(":GU#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":GU#") >= polls + 1; }, 3000));
    CHECK_NOTHROW((void)telescope->get_right_ascension());

    // The focuser joins again, still on the one link.
    focuser->set_connected(true);
    CHECK(focuser->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// A hangup is a lost link for every device on it (decision 0009 rule 2a): both read Connected false with no client
// I/O, both reconnect on their own Connect, and the reconnect is one link again.
// Falsified by: onstep_focuser_driver.cpp link_up() reading only the poll's loss latch, not the link's link_alive().
TEST_CASE("OnStep shared link - a link loss reaches both devices", "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);

    REQUIRE(controller.server.drop_connections());
    CHECK(alpacacore::test::wait_until([&] { return !telescope->get_connected(); }, 2000));
    CHECK_FALSE(focuser->get_connected());
    CHECK(focuser->get_link_fault().find("lost") != std::string::npos);
    require_alpaca_error([&] { (void)focuser->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { focuser->move(10); }, AlpacaError::NotConnected);

    focuser->set_connected(true);
    REQUIRE(focuser->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 2);  // the first link, then one fresh one for both
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// A refused command and its :GE# stay together on the shared link: a focuser poll that landed between them would
// replace the code, and a refused SetPark (:hQ# answered 0) would read the focuser command's code instead.
// Falsified by: onstep_telescope_driver.cpp set_park() without its hold_sequence().
TEST_CASE("OnStep shared link - a focuser poll never lands between a command and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        // SetPark refused, with a reason read by :GE# right after. The fake ends the bare digit with a '#', which
        // the driver's read takes at once, so a try costs no 100 ms idle gap.
        controller.state->set_park_reply = "0#";
        controller.state->ge_code = "12#";
    }
    constexpr int kSetParks = 300;  // the window between a reply and its :GE# is short: many tries to land in it
    controller.state->focuser_latency_ms = 0;  // the poll asks again as soon as it can
    std::atomic<bool> stop{false};
    std::thread hammer([&] {
        while (!stop.load()) {
            onstep::poll_onstep_focuser_now_for_testing(*focuser);
        }
    });
    int refused = 0;
    for (int i = 0; i < kSetParks; ++i) {
        try {
            telescope->set_park();
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation &&
                std::string(e.what()).find("no park position set") != std::string::npos) {
                ++refused;
            }
        }
    }
    stop.store(true);
    hammer.join();
    CHECK(refused == kSetParks);  // each refusal read its own code, never a focuser command's
    const auto log = controller.state->commands();
    int pairs = 0;
    for (std::size_t i = 1; i < log.size(); ++i) {
        if (log[i] == ":GE#") {
            ++pairs;
            CAPTURE(i);
            CHECK(log[i - 1] == ":hQ#");  // never a focuser command between the command and its :GE#
        }
    }
    CHECK(pairs == kSetParks);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

using alpacacore::test::onstep_controller::ge_pairs_after;
using alpacacore::test::onstep_controller::run_with_a_sibling_at_every_exchange;

// A refused telescope Unpark (:hR#) and its :GE# stay together on the shared link (#2223 review).
// Falsified by: onstep_telescope_driver.cpp unpark() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between an Unpark and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->unpark_reply = "0#";  // refused, with the reason read by :GE# right after
        controller.state->ge_code = "05#";
    }
    constexpr int kUnparks = 10;
    int refused = 0;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kUnparks, [&] {
        try {
            telescope->unpark();
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation) ++refused;
        }
    });
    CHECK(refused == kUnparks);
    CHECK(ge_pairs_after(log, ":hR#") == kUnparks);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// The focuser side: a refused Move (:F1s answered 0) and its :GE# stay together (#2223 review).
// Falsified by: onstep_focuser_driver.cpp move() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a focuser Move and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->goto_reply = "0#";  // a '#' ends the bare digit at once
        controller.state->ge_code = "09#";
    }
    constexpr int kMoves = 10;
    int refused = 0;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kMoves, [&] {
        try {
            focuser->move(3000);
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation) ++refused;
        }
    });
    CHECK(refused == kMoves);
    CHECK(ge_pairs_after(log, ":F1s") == kMoves);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// The other hold_sequence() sites, one case each (#2223 review): a refused command and its :GE# stay together on the
// shared link. Each sets the refusal, runs the action a few times with a sibling exchange before every exchange, and
// checks every :GE# follows the action's own command.
// Falsified by: onstep_telescope_driver.cpp set_tracking() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a Tracking write and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->reply_overrides[":Te"] = "0#";  // OnStepX refuses :Te#
        controller.state->ge_code = "05#";
    }
    constexpr int kTries = 5;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kTries, [&] {
        try {
            telescope->set_tracking(true);
        } catch (const alpacacore::AlpacaException&) {
        }
    });
    CHECK(ge_pairs_after(log, ":Te#") == kTries);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// Falsified by: onstep_telescope_driver.cpp set_rate_offset() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a rate-offset write and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->reply_overrides[":SXTR"] = "0#";  // the RA offset write is refused
        controller.state->ge_code = "05#";
    }
    constexpr int kTries = 5;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kTries, [&] {
        try {
            telescope->set_right_ascension_rate(1.0);
        } catch (const alpacacore::AlpacaException&) {
        }
    });
    CHECK(ge_pairs_after(log, ":SXTR") == kTries);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// Falsified by: onstep_telescope_driver.cpp set_tracking_rate() without the hold_sequence() of its offset zeroing.
TEST_CASE("OnStep shared link - no other exchange lands between a TrackingRate offset zeroing and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    telescope->set_right_ascension_rate(1.0);  // a non-zero offset, so a rate change zeroes it first
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->reply_overrides[":SXTR"] = "0#";  // the zeroing write is refused
        controller.state->ge_code = "05#";
    }
    const auto before = controller.state->commands().size();
    constexpr int kTries = 5;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kTries, [&] {
        try {
            telescope->set_tracking_rate(1);  // lunar: a change from the current rate
        } catch (const alpacacore::AlpacaException&) {
        }
    });
    CHECK(ge_pairs_after(std::vector<std::string>(log.begin() + static_cast<std::ptrdiff_t>(before), log.end()),
                         ":SXTR") == kTries);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// Falsified by: onstep_telescope_driver.cpp try_park_or_home_locked() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a Park and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->reply_overrides[":hP"] = "0#";  // refused, and not "in motion", so no retry
        controller.state->ge_code = "05#";
    }
    constexpr int kTries = 5;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kTries, [&] {
        try {
            telescope->park();
        } catch (const alpacacore::AlpacaException&) {
        }
    });
    CHECK(ge_pairs_after(log, ":hP#") == kTries);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// Falsified by: onstep_focuser_driver.cpp set_temp_comp() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a TempComp write and its :GE#",
          "[onstep][focuser][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    focuser->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(focuser->get_connected());
    REQUIRE(focuser->get_temp_comp_available());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->tcf_reply = "0#";  // refused
        controller.state->ge_code = "09#";
    }
    constexpr int kTries = 5;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kTries, [&] {
        try {
            focuser->set_temp_comp(true);
        } catch (const alpacacore::AlpacaException&) {
        }
    });
    CHECK(ge_pairs_after(log, ":F1c1#") == kTries);
    telescope->set_connected(false);
    focuser->set_connected(false);
}

// #2223 Testing Decisions: a focuser poll on the shared link never pushes a telescope pulse guide past its
// kPulseGuideSettle / kPulseMeasureBound timing (onstep_telescope_driver.cpp): with focuser exchanges queued on the
// wire throughout, IsPulseGuiding still reads true through the pulse and ends on the tick 50 ms after its end, which
// measures the post-pulse position. Falsified by: onstep_protocol_wrapper.cpp send_command() without its
// sequence_mutex_ and mutex_ lock_guards (a focuser reply then answers the telescope's :GRa# or :GU#).
TEST_CASE("OnStep shared link - a focuser poll keeps a telescope pulse guide inside its settle and measure bounds",
          "[onstep][focuser][shared-link]") {
    alpacacore::test::FakeTaskClock clock;
    Controller controller;
    auto info = controller.info();
    auto telescope = onstep::create_onstep_telescope_with_site(0, info, 40.5, -74.0, std::nullopt, false, clock);
    auto focuser = controller.focuser();
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    focuser->set_connected(true);
    REQUIRE(focuser->get_connected());
    onstep::set_onstep_focuser_poll_paused_for_testing(*focuser, true);
    REQUIRE(clock.wait_for_waiters(1, seconds(2)));  // the telescope poll waits for its first tick

    controller.state->focuser_latency_ms = 15;
    std::atomic<bool> stop{false};
    std::thread hammer([&] {
        while (!stop.load()) {
            onstep::poll_onstep_focuser_now_for_testing(*focuser);
        }
    });
    struct JoinHammer {
        std::atomic<bool>& stop;
        std::thread& thread;
        ~JoinHammer() {
            stop.store(true);
            if (thread.joinable()) thread.join();
        }
    } join_hammer{stop, hammer};

    for (int pulse = 0; pulse < 3; ++pulse) {
        CAPTURE(pulse);
        const auto waits = clock.wait_count();
        telescope->pulse_guide(2, 500);  // East, 0.5 s: the pulse-end tick waits end + kPulseGuideSettle
        REQUIRE(clock.wait_for_wait_count(waits + 1, seconds(2)));
        CHECK(telescope->get_is_pulse_guiding());
        const int position_reads = controller.state->count(":GRa#");

        clock.advance(milliseconds(500));  // the pulse's end: still guiding until a frame sampled 50 ms later
        CHECK(telescope->get_is_pulse_guiding());
        clock.advance(milliseconds(50));
        REQUIRE(clock.wait_for_woken_settled(seconds(5)));
        CHECK(controller.state->count(":GRa#") >= position_reads + 1);  // the measuring tick, behind the focuser's
        CHECK_FALSE(telescope->get_is_pulse_guiding());                 // well inside kPulseMeasureBound (2 s)
        // The tick read the telescope's own replies, not a focuser's: the position and the idle status.
        CHECK(telescope->get_right_ascension() == Catch::Approx(12.0));
        CHECK(telescope->get_declination() == Catch::Approx(45.0));
        CHECK_FALSE(telescope->get_slewing());
        CHECK_FALSE(telescope->get_tracking());
    }
    CHECK(focuser->get_position() == 1000);  // and the focuser's replies stayed its own
    stop.store(true);
    hammer.join();
    telescope->set_connected(false);
    focuser->set_connected(false);
}

#endif  // !_WIN32
