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

// The OnStepX auxiliary features as a Switch (#2222) on the controller's shared link: the telescope and the switch
// over one loopback FakeMountServer (the Network transport, onstep_controller.h). Replies follow OnStepX
// Features.command.cpp at af1d26bf (.github/instructions/onstep.instructions.md). The [stress] cases are in
// test_onstep_concurrency_stress.cpp.

#include <alpacacore/switch_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_switch_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

std::unique_ptr<alpacacore::SwitchDriver> make_offline_switch(int device_number) {
    onstep::ConnectionInfo conn;
    conn.type = onstep::ConnectionType::Serial;
    conn.port_path = "/dev/null";
    return onstep::create_onstep_switch(device_number, conn);
}

}  // namespace

TEST_CASE("OnStep Switch Driver - Defaults", "[onstep][switch][unit]") {
    auto driver = make_offline_switch(0);
    REQUIRE(driver->get_device_type() == DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_name() == "OnStep Switch");
    CHECK_FALSE(driver->get_device_firmware().has_value());
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("OnStep Switch Driver - Device metadata", "[onstep][switch][unit]") {
    auto driver = make_offline_switch(3);
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "OnStepX auxiliary features (LX200 protocol)");
    CHECK(driver->get_driver_info() == "AlpacaCore OnStep Switch Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "OnStep_Switch_3");
}

// The slots are read at connect, so MaxSwitch and every per-id member are NotConnected while disconnected.
// Falsified by: onstep_switch_driver.cpp get_max_switch() without its ensure_connected() (answers 0 disconnected).
TEST_CASE("OnStep Switch Driver - Unsupported members", "[onstep][switch][unit]") {
    auto driver = make_offline_switch(0);
    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));
    require_alpaca_error([&] { (void)driver->action("anything", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind(":GXY0#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_bool(":GXY0#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string(":GXY0#", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->get_max_switch(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_switch(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_switch_value(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_can_write(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_switch_name(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_switch(0, true); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_switch_value(0, 1.0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_async(0, true); }, AlpacaError::NotConnected);
}

// An id no OnStepX controller can have (slots 1-8: ids 0-7) and a value that is not a number are InvalidValue before
// the connection check. Falsified by: onstep_switch_driver.cpp check_static_id() accepting id 8.
TEST_CASE("OnStep Switch Driver - an id outside 0-7 or a non-finite value is InvalidValue while disconnected",
          "[onstep][switch][unit]") {
    auto driver = make_offline_switch(0);
    for (const int id : {-1, 8, 100}) {
        CAPTURE(id);
        require_alpaca_error([&] { (void)driver->get_switch(id); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { (void)driver->get_switch_name(id); }, AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->set_switch_value(id, 0.0); }, AlpacaError::InvalidValue);
    }
    require_alpaca_error([&] { driver->set_switch_value(0, std::numeric_limits<double>::quiet_NaN()); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(0, std::numeric_limits<double>::infinity()); },
                         AlpacaError::InvalidValue);
}

// The purpose table (Constants.h): 1 switch and 3 dew heater 0/1, 2 analog output 0-255, the rest left out.
// Falsified by: onstep_switch_driver.cpp aux_switch_kind() serving purpose 4 (intervalometer) as a 0/1 switch.
TEST_CASE("OnStep Switch Driver - which purposes a Switch serves", "[onstep][switch][unit]") {
    using onstep::detail::aux_switch_kind;
    REQUIRE(aux_switch_kind(1).has_value());
    CHECK(aux_switch_kind(1)->max == 1.0);
    REQUIRE(aux_switch_kind(2).has_value());
    CHECK(aux_switch_kind(2)->max == 255.0);
    REQUIRE(aux_switch_kind(3).has_value());
    CHECK(aux_switch_kind(3)->max == 1.0);
    for (const int purpose : {0, 4, 5, 6, 7, 8, -1}) {
        CAPTURE(purpose);
        CHECK_FALSE(aux_switch_kind(purpose).has_value());
    }
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

int count_writes(Controller& controller) { return controller.state->count(":SXX"); }

}  // namespace

// Ids 0..N-1 are the included slots in slot order (1 Camera, 2 Dew 1, 3 Flat; the intervalometer in 5 and the hidden
// switch in 6 are left out), under the firmware's names, with each purpose's range; values are the first field of
// :GXX<n>#, the power telemetry and the dew heater's other fields not read.
// Falsified by: onstep_protocol_wrapper.cpp aux_feature_value() parsing the whole reply (the ";12.1,..." suffix fails
// it).
TEST_CASE("OnStep Switch Driver - Device specifics once connected", "[onstep][switch][unit]") {
    Controller controller;
    auto driver = controller.aux_switch();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_max_switch() == 3);
    CHECK(driver->get_switch_name(0) == "Camera");
    CHECK(driver->get_switch_name(1) == "Dew 1");
    CHECK(driver->get_switch_name(2) == "Flat");
    CHECK(driver->get_switch_description(0) == "OnStepX auxiliary feature 1: on/off output");
    CHECK(driver->get_switch_description(1) == "OnStepX auxiliary feature 2: dew heater, enabled or disabled");
    CHECK(driver->get_switch_description(2) == "OnStepX auxiliary feature 3: PWM output, 0-255");
    for (int id = 0; id < 3; ++id) {
        CAPTURE(id);
        CHECK(driver->get_can_write(id));
        CHECK_FALSE(driver->get_can_async(id));
        CHECK(driver->get_min_switch_value(id) == 0.0);
        CHECK(driver->get_switch_step(id) == 1.0);
        CHECK(driver->get_state_change_complete(id));
    }
    CHECK(driver->get_max_switch_value(0) == 1.0);
    CHECK(driver->get_max_switch_value(1) == 1.0);
    CHECK(driver->get_max_switch_value(2) == 255.0);
    CHECK(driver->get_switch_value(0) == 0.0);
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch_value(1) == 1.0);
    CHECK(driver->get_switch(1));
    CHECK(driver->get_switch_value(2) == 128.0);
    CHECK(driver->get_switch(2));
    CHECK(driver->get_device_firmware() == std::optional<std::string>("On-Step 10.23a"));
    const auto state = driver->get_device_state();
    const auto has = [&](const std::string& name) {
        return std::any_of(state.begin(), state.end(), [&](const auto& e) { return e.name == name; });
    };
    CHECK(has("GetSwitchValue2"));
    CHECK(has("TimeStamp"));
    CHECK_FALSE(has("GetSwitchValue3"));
    driver->set_connected(false);
}

// A slot whose purpose the Switch does not serve is left out, an unknown code with an INFO naming the slot; a
// controller with nothing to serve is refused. Falsified by: onstep_switch_driver.cpp check_controller() without its
// slots.empty() refusal (Connect succeeds with MaxSwitch 0).
TEST_CASE("OnStep Switch Driver - features a Switch cannot serve are left out", "[onstep][switch][unit]") {
    Controller controller;
    SECTION("an unknown purpose is left out") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->features[4] = {"Future", 9, 0, ""};
        }
        auto driver = controller.aux_switch();
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(driver->get_max_switch() == 3);
        CHECK(controller.state->count(":GXX4#") == 0);  // never polled
        driver->set_connected(false);
    }
    SECTION("nothing to serve") {
        {
            std::lock_guard<std::mutex> lock(controller.state->mutex);
            controller.state->features = {{5, {"Intervalom", 4, 0, ""}}, {6, {"Hidden", 6, 1, ""}}};
        }
        auto driver = controller.aux_switch();
        CHECK(connect_error(*driver).find("no auxiliary feature a Switch can drive") != std::string::npos);
        CHECK_FALSE(driver->get_connected());
    }
}

// A write sends :SXX<slot>,V<v># for the id's slot (id 2 is slot 3), SetSwitch writes the maximum or 0, and the value
// is written through. Falsified by: onstep_switch_driver.cpp write_value() sending the switch id instead of
// slot.slot.
TEST_CASE("OnStep Switch Driver - writes go to the id's slot", "[onstep][switch][unit]") {
    Controller controller;
    auto driver = controller.aux_switch();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_switch_poll_paused_for_testing(*driver, true);
    driver->set_switch_value(2, 200.0);
    CHECK(controller.state->count(":SXX3,V200#") == 1);
    CHECK(driver->get_switch_value(2) == 200.0);
    driver->set_switch(2, false);
    CHECK(controller.state->count(":SXX3,V0#") == 1);
    CHECK(driver->get_switch_value(2) == 0.0);
    driver->set_switch(0, true);
    CHECK(controller.state->count(":SXX1,V1#") == 1);
    CHECK(driver->get_switch(0));
    driver->set_switch_value(1, 0.0);
    CHECK(controller.state->count(":SXX2,V0#") == 1);
    CHECK_FALSE(driver->get_switch(1));
    driver->set_switch_value(2, 99.6);  // the step is 1: rounded, as the firmware rounds
    CHECK(controller.state->count(":SXX3,V100#") == 1);
    driver->set_connected(false);
}

// A value outside the slot's range, and an id past MaxSwitch, are InvalidValue with nothing sent.
// Falsified by: onstep_switch_driver.cpp write_value() without its range check (sends :SXX1,V2#).
TEST_CASE("OnStep Switch Driver - a value outside the range is InvalidValue with nothing sent",
          "[onstep][switch][unit]") {
    Controller controller;
    auto driver = controller.aux_switch();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    require_alpaca_error([&] { driver->set_switch_value(0, 2.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(1, -1.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(2, 256.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(3, 0.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_switch(3); }, AlpacaError::InvalidValue);
    CHECK(count_writes(controller) == 0);
    // Async control and renaming are not supported, after the id and connection checks.
    require_alpaca_error([&] { driver->set_async(0, true); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->set_async_value(0, 1.0); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->set_switch_name(0, "x"); }, AlpacaError::MethodNotImplemented);
    driver->set_connected(false);
}

// A write the controller refuses ('0') throws InvalidOperation with the :GE# reason, read right after it.
// Falsified by: onstep_switch_driver.cpp write_value() ignoring the refused set_aux_feature_value() reply.
TEST_CASE("OnStep Switch Driver - a refused write reports the controller's reason", "[onstep][switch][unit]") {
    Controller controller;
    auto driver = controller.aux_switch();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->feature_set_reply = "0";
        controller.state->ge_code = "04#";  // CE_PARAM_RANGE
    }
    std::string message;
    try {
        driver->set_switch(0, true);
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidOperation);
        message = e.what();
    }
    CHECK(message.find("parameter out of range") != std::string::npos);
    CHECK_FALSE(driver->get_switch(0));
    const auto log = controller.state->commands();
    const auto write_at = std::find(log.begin(), log.end(), ":SXX1,V1#");
    REQUIRE(write_at != log.end());
    REQUIRE(write_at + 1 != log.end());
    CHECK(*(write_at + 1) == ":GE#");
    driver->set_connected(false);
}

// A change made at the controller (the OnStepX web page, a momentary switch turning itself off) shows up at the next
// poll. Falsified by: onstep_switch_driver.cpp probe() keeping the connect's values instead of reading :GXX<n>#.
TEST_CASE("OnStep Switch Driver - a change at the controller shows up after the poll", "[onstep][switch][unit]") {
    Controller controller;
    auto driver = controller.aux_switch();
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_switch_poll_paused_for_testing(*driver, true);
    CHECK(driver->get_switch_value(2) == 128.0);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->features[3].value = 17;
        controller.state->features[1].value = 1;
    }
    onstep::poll_onstep_switch_now_for_testing(*driver);
    CHECK(driver->get_switch_value(2) == 17.0);
    CHECK(driver->get_switch(0));
    driver->set_connected(false);
}

// Connect refuses OnStep 4.x and holds no link after.
// Falsified by: onstep_switch_driver.cpp check_controller() without its is_onstepx_version() check.
TEST_CASE("OnStep Switch Driver - Connect refuses OnStep 4.x", "[onstep][switch][unit]") {
    Controller controller;
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->version = "4.24s";
    }
    auto driver = controller.aux_switch();
    CHECK(connect_error(*driver).find("requires OnStepX firmware") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(controller.state->count(":GXY") == 0);  // nothing sent to a firmware the driver has not checked
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// Decision 0010: DeviceState's TimeStamp is when the served frame was measured. Falsified by:
// onstep_switch_driver.cpp device_state_age() delete the override (the TimeStamp is the request time).
TEST_CASE("OnStep Switch Driver - DeviceState TimeStamp is when the frame was measured",
          "[onstep][switch][snapshot][unit]") {
    alpacacore::test::FakeTaskClock clock;
    Controller controller;
    auto driver = onstep::create_onstep_switch(0, controller.info(), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    onstep::set_onstep_switch_poll_paused_for_testing(*driver, true);
    onstep::poll_onstep_switch_now_for_testing(*driver);  // a frame measured at the fake clock's now
    clock.advance(std::chrono::milliseconds(1500));       // paused: no poll, the frame ages

    std::string stamp;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "TimeStamp") stamp = std::get<std::string>(entry.value);
    }
    const auto taken = std::chrono::system_clock::now();
    REQUIRE_FALSE(stamp.empty());
    const auto stamped = alpacacore::test::parse_timestamp(stamp);
    const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(taken - stamped);
    INFO("TimeStamp is " << behind.count() << " ms behind now");
    CHECK(behind >= std::chrono::milliseconds(1400));  // the 1500 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));
    driver->set_connected(false);
}

// ── The shared link (#2222) ──────────────────────────────────────────────────────────────────────────────────────

// Falsified by: onstep_switch_driver.cpp release_link() calling link->disconnect() before letting go (the telescope
// loses the link with the switch's Disconnect).
TEST_CASE("OnStep shared link - telescope then switch share one link", "[onstep][switch][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto aux = controller.aux_switch();
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    aux->set_connected(true);
    REQUIRE(aux->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    CHECK(aux->get_switch_value(2) == 128.0);

    aux->set_connected(false);
    CHECK_FALSE(aux->get_connected());
    CHECK(telescope->get_connected());
    const int polls = controller.state->count(":GU#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":GU#") >= polls + 1; }, 3000));
    CHECK_NOTHROW((void)telescope->get_right_ascension());

    aux->set_connected(true);
    CHECK(aux->get_connected());
    CHECK(controller.server.accepted_connections() == 1);
    telescope->set_connected(false);
    aux->set_connected(false);
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// Falsified by: onstep_switch_driver.cpp open_locked() opening its own link (OnStepProtocolWrapper{} and connect())
// instead of acquire().
TEST_CASE("OnStep shared link - switch then telescope share one link", "[onstep][switch][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto aux = controller.aux_switch();
    aux->set_connected(true);
    REQUIRE(aux->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 1);

    telescope->set_connected(false);
    CHECK_FALSE(telescope->get_connected());
    CHECK(aux->get_connected());
    const int polls = controller.state->count(":GXX1#");
    CHECK(alpacacore::test::wait_until([&] { return controller.state->count(":GXX1#") >= polls + 2; }, 4000));
    aux->set_switch(0, true);
    CHECK(controller.state->count(":SXX1,V1#") == 1);
    CHECK(controller.server.accepted_connections() == 1);
    aux->set_connected(false);
    CHECK(onstep::detail::registered_link_for_testing(controller.info()) == nullptr);
}

// A hangup is a lost link for every device on it (decision 0009 rule 2a).
// Falsified by: onstep_switch_driver.cpp link_up() reading only the poll's loss latch, not the link's link_alive().
TEST_CASE("OnStep shared link - a link loss reaches the telescope and the switch", "[onstep][switch][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto aux = controller.aux_switch();
    telescope->set_connected(true);
    aux->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(aux->get_connected());
    onstep::set_onstep_switch_poll_paused_for_testing(*aux, true);

    REQUIRE(controller.server.drop_connections());
    CHECK(alpacacore::test::wait_until([&] { return !telescope->get_connected(); }, 2000));
    CHECK_FALSE(aux->get_connected());
    CHECK(aux->get_link_fault().find("lost") != std::string::npos);
    require_alpaca_error([&] { (void)aux->get_switch_value(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { aux->set_switch(0, true); }, AlpacaError::NotConnected);

    aux->set_connected(true);
    REQUIRE(aux->get_connected());
    telescope->set_connected(true);
    REQUIRE(telescope->get_connected());
    CHECK(controller.server.accepted_connections() == 2);
    telescope->set_connected(false);
    aux->set_connected(false);
}

// A refused switch write (:SXX answered 0) and its :GE# stay together on the shared link.
// Falsified by: onstep_switch_driver.cpp write_value() without its hold_sequence().
TEST_CASE("OnStep shared link - no other exchange lands between a switch write and its :GE#",
          "[onstep][switch][shared-link]") {
    Controller controller;
    auto telescope = controller.telescope();
    auto aux = controller.aux_switch();
    telescope->set_connected(true);
    aux->set_connected(true);
    REQUIRE(telescope->get_connected());
    REQUIRE(aux->get_connected());
    onstep::set_onstep_switch_poll_paused_for_testing(*aux, true);
    onstep::set_status_poll_paused_for_testing(*telescope, true);
    {
        std::lock_guard<std::mutex> lock(controller.state->mutex);
        controller.state->feature_set_reply = "0#";  // a '#' ends the bare digit at once
        controller.state->ge_code = "04#";
    }
    constexpr int kWrites = 10;
    int refused = 0;
    const auto log = run_with_a_sibling_at_every_exchange(controller, kWrites, [&] {
        try {
            aux->set_switch_value(2, 50.0);
        } catch (const alpacacore::AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation) ++refused;
        }
    });
    CHECK(refused == kWrites);
    CHECK(ge_pairs_after(log, ":SXX3") == kWrites);
    telescope->set_connected(false);
    aux->set_connected(false);
}

#endif  // !_WIN32
