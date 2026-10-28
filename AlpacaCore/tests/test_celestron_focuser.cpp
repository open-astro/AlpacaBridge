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

// Celestron Focus Motor on its own USB port (issue #896), against a PTY fake of
// AUX device 0x12. The driver runs on a FakeTaskClock, so a poll tick is an
// advance(), not a sleep.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

#ifndef _WIN32
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "fake_celestron_focuser.h"
#include "fake_task_clock.h"
#endif

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::vendor::celestron::create_celestron_focuser;
using namespace std::chrono_literals;

namespace {

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("Celestron Focuser Driver - Defaults", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "Celestron Focus Motor");
    CHECK(driver->get_absolute() == true);
}

TEST_CASE("Celestron Focuser Driver - Device metadata", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Celestron Focus Motor (USB)");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "CELESTRON_FOCUSER_3");
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Celestron Focuser Driver - Disconnected behavior", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_max_step(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_max_increment(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temp_comp_available(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temp_comp(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_temp_comp(false); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->halt(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::NotConnected);
    // Platform 7: a disconnected driver reports an empty DeviceState.
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("Celestron Focuser Driver - Unsupported actions", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions() ==
          std::vector<std::string>{"Calibrate", "AbortCalibration", "CalibrationStatus"});
    CHECK(driver->can_action("calibrate"));  // ASCOM action names are case-insensitive
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&] { driver->action("noop", ""); }, AlpacaError::ActionNotImplemented);
    for (const char* name : {"Calibrate", "AbortCalibration", "CalibrationStatus"}) {
        INFO(name);
        require_alpaca_error([&] { driver->action(name, ""); }, AlpacaError::NotConnected);
    }
    require_alpaca_error([&] { driver->command_blind("noop", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_bool("noop", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_string("noop", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("Celestron Focuser Driver - Connect to a missing port fails", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/alpacabridge-no-such-celestron-focuser");
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK(driver->get_connected() == false);
}

#ifndef _WIN32

using alpacacore::test::FakeCelestronFocuser;
using alpacacore::test::FakeTaskClock;

namespace {

constexpr auto kTick = 300ms;  // longer than the 250 ms poll period
constexpr auto kBound = 5000ms;

void tick(FakeTaskClock& clock) {
    clock.advance(kTick);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

}  // namespace

TEST_CASE("Celestron Focuser Driver - Position and MaxStep are referenced to the high limit",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    fake.set_raw_position(30000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_max_step() == 60000);
    CHECK(driver->get_max_increment() == 60000);
    CHECK(driver->get_position() == 31000);  // 61000 - 30000
    CHECK(driver->get_is_moving() == false);

    fake.set_raw_position(61000);
    tick(clock);
    CHECK(driver->get_position() == 0);
    fake.set_raw_position(1000);
    tick(clock);
    CHECK(driver->get_position() == 60000);

    // Every request goes from the application address to the focus motor.
    for (const auto& frame : fake.requests()) {
        CHECK(frame.source == 0x20);
        CHECK(frame.destination == 0x12);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - the motor's USB port runs at 9600 baud", "[celestron][focuser][unit][fake]") {
    // The PTY fake answers at any line rate, so only the termios the driver
    // set shows a connect that would never reach a real motor (115200, the
    // CGX mount's speed). An observer opened before the connect reads it back.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    const int observer = ::open(fake.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(observer >= 0);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    struct termios settings {};
    REQUIRE(::tcgetattr(observer, &settings) == 0);
    CHECK(::cfgetospeed(&settings) == B9600);
    CHECK(::cfgetispeed(&settings) == B9600);
    driver->set_connected(false);
    ::close(observer);
}

TEST_CASE("Celestron Focuser Driver - holds its port under its own name", "[celestron][focuser][unit][fake]") {
    // The focuser shares the CGX AUX transport; the registry holder is what a
    // refused operator reads (#1202, decision 0017), so it must name the focuser.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    const auto holder = alpacacore::util::serial_port_holder(fake.slave_path());
    REQUIRE(holder.has_value());
    CHECK(holder->owner == "Celestron focuser");
    driver->set_connected(false);
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("Celestron Focuser Driver - Move sends the raw target and IsMoving follows the motor",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(5000);
    REQUIRE(fake.last_goto().has_value());
    CHECK(*fake.last_goto() == 56000U);  // 61000 - 5000
    // Write-through: true before the next poll.
    CHECK(driver->get_is_moving() == true);
    tick(clock);
    CHECK(driver->get_is_moving() == true);

    fake.complete_move();
    tick(clock);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == 5000);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Move clamps to the calibrated range", "[celestron][focuser][unit][fake]") {
    // ConformU expects graceful clamping for Move outside 0..MaxStep, as the
    // other focuser drivers do, rather than InvalidValue.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(-50);
    CHECK(*fake.last_goto() == 61000U);
    driver->move(999999);
    CHECK(*fake.last_goto() == 1000U);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Halt stops the motor", "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(100);
    REQUIRE(fake.moving());
    driver->halt();
    CHECK(fake.count(0x24) == 1);
    const auto requests = fake.requests();
    REQUIRE(requests.back().command == 0x24);
    CHECK(requests.back().payload == std::vector<std::uint8_t>{0x00});
    CHECK_FALSE(fake.moving());
    CHECK(driver->get_is_moving() == false);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - an uncalibrated motor connects but refuses positions and moves",
          "[celestron][focuser][unit][fake]") {
    // Issue #896: the Celestron utilities can wipe the calibration, so the
    // connect stays open for the Calibrate action instead of refusing.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(5000, 5000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_connected() == true);
    CHECK(driver->action("CalibrationStatus", "") == "NotCalibrated");
    CHECK(driver->get_is_moving() == false);
    for (const auto& [name, call] : std::vector<std::pair<const char*, std::function<void()>>>{
             {"Position", [&] { (void)driver->get_position(); }},
             {"MaxStep", [&] { (void)driver->get_max_step(); }},
             {"MaxIncrement", [&] { (void)driver->get_max_increment(); }},
             {"Move", [&] { driver->move(100); }}}) {
        INFO(name);
        try {
            call();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("not calibrated") != std::string::npos);
        }
    }
    CHECK(fake.count(0x02) == 0);
    bool has_position = false;
    for (const auto& entry : driver->get_device_state()) {
        has_position = has_position || entry.name == "Position";
    }
    CHECK_FALSE(has_position);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Calibrate runs to completion and the new limits take effect",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(0, 0);
    fake.set_raw_position(30000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->action("Calibrate", "") == "");
    const auto requests = fake.requests();
    REQUIRE(requests.back().command == 0x2a);
    CHECK(requests.back().payload == std::vector<std::uint8_t>{0x01});
    CHECK(fake.calibrating());
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");
    CHECK(driver->get_is_moving() == true);
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::InvalidOperation);

    for (int i = 0; i < 5; ++i) tick(clock);  // past the start grace: the status is polled
    CHECK(fake.count(0x2b) > 0);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");
    CHECK(driver->get_is_moving() == true);

    fake.complete_calibration(2000, 52000);
    tick(clock);
    tick(clock);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrated");
    CHECK(driver->get_max_step() == 50000);
    CHECK(driver->get_position() == 22000);  // 52000 - 30000
    CHECK(driver->get_is_moving() == false);
    driver->move(1000);
    CHECK(*fake.last_goto() == 51000U);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Halt and AbortCalibration abort a calibration and never wipe an idle motor",
          "[celestron][focuser][unit][fake]") {
    // The reporter saw 0x2A 00 wipe a good calibration (issue #896), so it is
    // sent only while a calibration the driver tracks is running.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->action("AbortCalibration", "") == "");
    driver->halt();
    tick(clock);
    CHECK(fake.count(0x2a) == 0);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrated");
    CHECK(driver->get_max_step() == 60000);

    SECTION("Halt") {
        driver->action("Calibrate", "");
        driver->halt();
    }
    SECTION("AbortCalibration") {
        driver->action("Calibrate", "");
        driver->action("AbortCalibration", "");
    }
    const auto requests = fake.requests();
    REQUIRE(requests.back().command == 0x2a);
    CHECK(requests.back().payload == std::vector<std::uint8_t>{0x00});
    CHECK_FALSE(fake.calibrating());
    CHECK(driver->get_is_moving() == false);
    tick(clock);
    // The abort wiped the motor's limits; the driver reads them again.
    CHECK(driver->action("CalibrationStatus", "") == "NotCalibrated");
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::InvalidOperation);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - an aborted calibration serves no old limits before the poll reads them",
          "[celestron][focuser][unit][fake]") {
    // The abort wipes the motor's limits (issue #896) and the driver reads
    // them again only on its next poll, up to 250 ms later. Until then a Move
    // must not GOTO against the limits the motor had before.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    REQUIRE(driver->get_max_step() == 60000);

    driver->action("Calibrate", "");
    driver->action("AbortCalibration", "");
    // No tick: the poll has not read the wiped limits yet.
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_max_step()); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_max_increment()); }, AlpacaError::InvalidOperation);
    require_alpaca_error([&] { static_cast<void>(driver->get_position()); }, AlpacaError::InvalidOperation);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");
    CHECK(fake.count(0x02) == 0);

    tick(clock);
    CHECK(driver->action("CalibrationStatus", "") == "NotCalibrated");
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::InvalidOperation);
    CHECK(fake.count(0x02) == 0);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a Halt racing the poll that sees the calibration finish never wipes it",
          "[celestron][focuser][unit][fake]") {
    // The poll's 0x2B can report the calibration complete between Halt's
    // running check and its 0x2A 00, which then reaches an idle motor and
    // wipes the new calibration (issue #896).
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(0, 0);
    fake.set_raw_position(30000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    driver->action("Calibrate", "");
    for (int i = 0; i < 5; ++i) tick(clock);  // past the start grace
    REQUIRE(fake.count(0x2b) > 0);

    // The next poll's 0x2B is held unanswered while the calibration still runs.
    fake.gate_next(0x2b);
    clock.advance(kTick);
    REQUIRE(fake.wait_gate_reached(kBound));
    std::thread halter([&] { driver->halt(); });
    // Halt reaches its lock before the reply; well inside the 500 ms reply timeout.
    std::this_thread::sleep_for(100ms);
    fake.complete_calibration(2000, 52000);
    fake.release_gate();
    halter.join();
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
    tick(clock);

    const auto requests = fake.requests();
    CHECK(std::none_of(requests.begin(), requests.end(), [](const auto& frame) {
        return frame.command == 0x2a && frame.payload == std::vector<std::uint8_t>{0x00};
    }));
    CHECK(driver->action("CalibrationStatus", "") == "Calibrated");
    CHECK(driver->get_max_step() == 50000);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a reconnect during a calibration keeps tracking it",
          "[celestron][focuser][unit][fake]") {
    // Disconnect does not abort a calibration. The reconnect used to start
    // from "not running" and serve the mid-sweep limits as a calibrated motor.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    driver->action("Calibrate", "");
    driver->set_connected(false);
    REQUIRE(fake.calibrating());

    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");
    CHECK(driver->get_is_moving() == true);
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::InvalidOperation);
    driver->action("AbortCalibration", "");
    const auto requests = fake.requests();
    REQUIRE(requests.back().command == 0x2a);
    CHECK(requests.back().payload == std::vector<std::uint8_t>{0x00});
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - an unconfirmed calibration start warns that the motor may be calibrating",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    std::vector<std::string> warnings;
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
        ~SinkGuard() {
            alpacacore::logging::set_log_sink(previous);
            alpacacore::logging::set_log_level(level);
        }
    } sink_guard;
    alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Warn);
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn && component == "Celestron") {
                warnings.emplace_back(message);
            }
        });
    fake.drop_next_reply(0x2a);  // the start reaches the motor; its reply is lost
    CHECK_THROWS(driver->action("Calibrate", ""));
    CHECK(fake.calibrating());
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& message) {
        return message.find("may be calibrating") != std::string::npos;
    }));
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a calibration status still idle right after the start is not an abort",
          "[celestron][focuser][unit][fake]") {
    // INDI waits after the start before it reads 0x2B; a status read at once
    // could still say idle and end the calibration the motor is running.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    const int read_at_connect = fake.count(0x2b);  // the connect's own status read
    fake.hold_calibration_status_idle(true);
    driver->action("Calibrate", "");
    tick(clock);
    CHECK(fake.count(0x2b) == read_at_connect);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");

    fake.hold_calibration_status_idle(false);
    for (int i = 0; i < 4; ++i) tick(clock);
    CHECK(fake.count(0x2b) > read_at_connect);
    CHECK(driver->action("CalibrationStatus", "") == "Calibrating");
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Calibrate is refused while the motor moves or a calibration runs",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(100);
    require_alpaca_error([&] { driver->action("Calibrate", ""); }, AlpacaError::InvalidOperation);
    CHECK(fake.count(0x2a) == 0);

    fake.complete_move();
    tick(clock);
    driver->action("Calibrate", "");
    require_alpaca_error([&] { driver->action("Calibrate", ""); }, AlpacaError::InvalidOperation);
    CHECK(fake.count(0x2a) == 1);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - capabilities, firmware and DeviceState when connected",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_version({7, 11, 0x0b, 0xb8});
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("7.11.3000"));

    bool has_position = false;
    bool has_moving = false;
    bool has_temperature = false;
    for (const auto& entry : driver->get_device_state()) {
        has_position = has_position || entry.name == "Position";
        has_moving = has_moving || entry.name == "IsMoving";
        has_temperature = has_temperature || entry.name == "Temperature";
    }
    CHECK(has_position);
    CHECK(has_moving);
    CHECK_FALSE(has_temperature);  // no sensor: omitted, not invented

    driver->set_connected(false);
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Celestron Focuser Driver - unsupported members report not implemented", "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_temp_comp(true); }, AlpacaError::NotImplemented);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a getter right after a connect whose first poll was lost has an answer",
          "[celestron][focuser][unit][fake]") {
    // The connect's first poll can publish nothing: its reply is lost. Connect
    // may do I/O, so it retries once before Connected reads true; the getters
    // then answer from that frame and run no exchange of their own (decisions
    // 0010 and 0016).
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.drop_next_reply(0x13);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const int before = fake.count(0x01);

    CHECK(driver->get_position() == 31000);
    CHECK(driver->get_is_moving() == false);
    CHECK(fake.count(0x01) == before);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - getters on a stalled link add no AUX exchanges and never trip the latch",
          "[celestron][focuser][unit][fake]") {
    // Getters read the snapshot and never wait on the wire (decisions 0010 and
    // 0016). The first connect poll is lost and its retry publishes the frame;
    // with the motor then silent, getters serve that frame without an exchange
    // of their own, so the 250 ms poll cadence alone decides when the latch
    // (threshold 3) trips.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.drop_next_reply(0x13);  // the connect poll; its retry publishes
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    const int before = fake.count(0x01);

    fake.set_muted(true);
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_position() == 31000);
        CHECK_FALSE(driver->get_is_moving());
    }
    CHECK(fake.count(0x01) == before);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Halt and Move resynchronize a timed-out link first",
          "[celestron][focuser][unit][fake]") {
    // One lost poll reply leaves the AUX stream desynchronized until the next
    // poll. Halt is the safety stop and must still reach the motor; Move must
    // not fail on the stale state either. Neither resends a timed-out frame.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    fake.set_muted(true);
    tick(clock);  // this poll times out: the link is now desynchronized
    fake.set_muted(false);

    driver->halt();
    CHECK(fake.count(0x24) == 1);

    fake.set_muted(true);
    tick(clock);
    fake.set_muted(false);

    driver->move(5000);
    REQUIRE(fake.last_goto().has_value());
    CHECK(*fake.last_goto() == 56000U);
    CHECK(fake.count(0x02) == 1);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a silent motor faults the link and a reply restores it",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    fake.set_muted(true);
    for (int i = 0; i < 3; ++i) tick(clock);
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { driver->move(10); }, AlpacaError::DriverException);
    CHECK(driver->get_connected() == true);

    // The poll resynchronizes the timed-out stream before asking again.
    fake.set_muted(false);
    tick(clock);
    CHECK(driver->get_position() == 31000);
    driver->set_connected(false);
}

// Falsified by: celestron_focuser_driver.cpp: set_connected() drop the connect's first-frame check (a connect whose
// first state poll was lost succeeds with no frame).
TEST_CASE("Celestron Focuser Driver - a getter after a successful connect never throws DriverException",
          "[celestron][focuser][snapshot][unit]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.drop_next_reply(0x13, 2);  // the connect poll and its retry
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    // Both first polls are lost: the connect is refused, so no getter can run on a
    // session without a frame, and the refused connect leaves nothing behind.
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    // The port was released: a connect with replies back succeeds and serves a frame.
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK_NOTHROW(driver->get_position());
    CHECK_NOTHROW(driver->get_is_moving());
    driver->set_connected(false);
}

// Falsified by: celestron_focuser_driver.cpp: Connected read connected_ alone (drop the !state_.link_lost() term), so a
// fault standing past util::kLinkStalenessBound left Connected true and the getters on a latched DriverException.
TEST_CASE("Celestron Focuser Driver - a fault that stands past the link-loss bound drops Connected",
          "[celestron][focuser][snapshot][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    REQUIRE(driver->get_position() == 31000);
    CHECK(alpacacore::util::is_serial_port_in_use(fake.slave_path()));

    // Three failed polls latch the fault; Connected is still true (decision 0009).
    fake.set_muted(true);
    for (int i = 0; i < 3; ++i) tick(clock);
    CHECK(driver->get_connected() == true);

    // The fault stands past util::kLinkStalenessBound (30 s): the link is lost.
    clock.advance(31s);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    CHECK(driver->get_connected() == false);
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move(10); }, AlpacaError::NotConnected);
    CHECK(driver->get_device_state().empty());
    // on_lost() released the transport.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(fake.slave_path()));

    // No autonomous relink; the next Connect re-runs the probe and serves a fresh frame.
    fake.set_muted(false);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(driver->get_position() == 31000);
    driver->set_connected(false);
}

// Falsified by: celestron_focuser_driver.cpp: get_link_fault() delete the override (the management listing never says
// why the link is down).
TEST_CASE(
    "Celestron Focuser Driver - the link fault text is reported while faulted and after the loss, and cleared by "
    "Disconnect",
    "[celestron][focuser][snapshot][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    CHECK(driver->get_link_fault().empty());

    fake.set_muted(true);
    for (int i = 0; i < 3; ++i) tick(clock);
    CHECK_FALSE(driver->get_link_fault().empty());  // latched, Connected still true

    clock.advance(31s);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    CHECK(driver->get_connected() == false);
    CHECK_FALSE(driver->get_link_fault().empty());  // kept after the loss

    driver->set_connected(false);
    CHECK(driver->get_link_fault().empty());
}

// Falsified by: celestron_focuser_driver.cpp: device_state_age() delete the override (the TimeStamp is the request
// time, not the frame's).
TEST_CASE("Celestron Focuser Driver - DeviceState TimeStamp is when the frame was measured",
          "[celestron][focuser][snapshot][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));
    tick(clock);                                   // one settled poll: the next is a full period away
    clock.advance(std::chrono::milliseconds(90));  // under the poll period: no poll, the frame ages
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
    CHECK(behind >= std::chrono::milliseconds(60));  // the 90 ms frame age; a request-time stamp is ~0
    CHECK(behind < std::chrono::milliseconds(5000));
    driver->set_connected(false);
}

#endif  // _WIN32
