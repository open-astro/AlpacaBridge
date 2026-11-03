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

// The iOptron iFR rotator (CAA) half over FakeIoptronIfr (fake_ioptron_ifr.h). The fake models the frame the
// firmware reports, measured on an iFR 3" on 2026-11-03: Position = mechanical + offset, mirrored to
// 360 - (mechanical + offset) when reversed, and a :RY sent while reversed storing an offset that does not
// match the mirrored frame. Cases 3 and 4 come from the contract sweep entry (ifr_rotator).

#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_rotator_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_ioptron_ifr.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeIoptronIfr;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::require_alpaca_error;
namespace ifr = alpacacore::vendor::ioptron;

constexpr auto kPastTtl = std::chrono::milliseconds(150);
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::unique_ptr<alpacacore::RotatorDriver> connected_rotator(FakeIoptronIfr& fake, FakeTaskClock& clock) {
    auto driver = ifr::create_ifr_rotator(0, fake.slave_path(), "ifr3", clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

void next_poll(FakeTaskClock& clock) { clock.advance(kPastTtl); }

// Polls until the move is over (each poll past the cache); the fake's move takes a few polls.
void settle(alpacacore::RotatorDriver& d, FakeTaskClock& clock) {
    for (int i = 0; i < 20 && d.get_is_moving(); ++i) next_poll(clock);
    REQUIRE_FALSE(d.get_is_moving());
    next_poll(clock);
}

}  // namespace

TEST_CASE("iOptron iFR Rotator Driver - Defaults", "[ioptron][ifr][rotator][unit]") {
    auto driver = ifr::create_ifr_rotator(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Rotator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "iOptron iFR 3\" Rotator");
    CHECK(driver->get_can_reverse() == true);
    CHECK(ifr::create_ifr_rotator(0, "/dev/ttyUSB0", "ifr2")->get_name() == "iOptron iFR 2\" Rotator");
}

TEST_CASE("iOptron iFR Rotator Driver - Device metadata", "[ioptron][ifr][rotator][unit]") {
    auto driver = ifr::create_ifr_rotator(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iFR 3\" focuser-rotator, rotator (CAA)");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iFR Rotator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "IOPTRON_IFR_ROTATOR_3");
}

TEST_CASE("iOptron iFR Rotator Driver - Device specifics once connected", "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_rotator(fake, clock);

    CHECK(driver->get_step_size() == Catch::Approx(0.004));
    CHECK(driver->get_position() == Catch::Approx(35.0));
    CHECK(driver->get_mechanical_position() == Catch::Approx(35.0));
    CHECK(driver->get_reverse() == false);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("260324"));

    std::vector<std::string> names;
    for (const auto& s : driver->get_device_state()) names.push_back(s.name);
    CHECK(names == std::vector<std::string>{"IsMoving", "MechanicalPosition", "Position", "TimeStamp"});
}

TEST_CASE("iOptron iFR Rotator Driver - Value range validation", "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_rotator(fake, clock);

    require_alpaca_error([&] { driver->move_absolute(kNaN); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->move_mechanical(std::numeric_limits<double>::infinity()); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->move(kNaN); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->sync(kNaN); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_target_position(kNaN); }, AlpacaError::InvalidValue);
    CHECK(fake.count(":RM") == 0);
    CHECK(fake.count(":RY") == 0);

    // An angle outside 0..360 is wrapped, as the ConformU-validated rotators do.
    fake.set_rotator_polls(0);
    driver->move_absolute(370.0);
    CHECK(fake.count(":RM003600000#") == 1);  // 10 degrees
    settle(*driver, clock);
    CHECK(driver->get_position() == Catch::Approx(10.0));

    // InvalidValue precedes NotConnected.
    auto offline = ifr::create_ifr_rotator(0, "/dev/ttyUSB0");
    require_alpaca_error([&] { offline->move_absolute(kNaN); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { offline->move_absolute(10.0); }, AlpacaError::NotConnected);
}

TEST_CASE("iOptron iFR Rotator Driver - State machine", "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    fake.set_rotator_polls(2);
    auto driver = connected_rotator(fake, clock);

    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_target_position() == 0.0);  // no move commanded yet

    driver->move_absolute(90.0);
    CHECK(driver->get_target_position() == Catch::Approx(90.0));
    CHECK(driver->get_is_moving() == true);
    next_poll(clock);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == Catch::Approx(90.0));

    SECTION("the hold keeps IsMoving true until the motor starts") {
        fake.set_start_delay_polls(2);
        driver->move_absolute(120.0);
        CHECK(driver->get_is_moving() == true);  // still at 90: the hold answers
        CHECK(driver->get_position() == Catch::Approx(90.0));
        next_poll(clock);
        CHECK(driver->get_is_moving() == true);
        settle(*driver, clock);
        CHECK(driver->get_position() == Catch::Approx(120.0));
    }

    SECTION("Halt stops the move and ends the hold") {
        fake.set_start_delay_polls(1 << 20);
        driver->move_absolute(200.0);
        driver->halt();
        CHECK(fake.count(":RQ#") == 1);
        CHECK(driver->get_is_moving() == false);
        CHECK(driver->get_position() == Catch::Approx(90.0));
    }

    SECTION("Move is relative to the current Position and wraps") {
        driver->move(300.0);
        settle(*driver, clock);
        CHECK(driver->get_position() == Catch::Approx(30.0));
        CHECK(driver->get_target_position() == Catch::Approx(30.0));
    }
}

TEST_CASE("iOptron iFR Rotator Driver - Unsupported members", "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_rotator(fake, clock);

    // Every IRotatorV4 member is implemented; the vendor-only commands are not reachable through pass-through.
    require_alpaca_error([&] { driver->command_blind(":RT4#", true); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->command_string(":RG#", true); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { (void)driver->action("SetSize", "30"); }, AlpacaError::ActionNotImplemented);
    CHECK(fake.count(":RT") == 0);
}

TEST_CASE("iOptron iFR Rotator Driver - Sync is stored in the device", "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_rotator(fake, clock);

    driver->sync(100.0);
    CHECK(fake.count(":RY036000000#") == 1);
    CHECK(fake.rotator_offset() == Catch::Approx(65.0));
    next_poll(clock);
    CHECK(driver->get_position() == Catch::Approx(100.0));
    CHECK(driver->get_mechanical_position() == Catch::Approx(35.0));  // Sync moves nothing
    CHECK(driver->get_target_position() == Catch::Approx(100.0));

    // A reconnect reads the offset back from the device: nothing kept in the driver.
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(driver->get_position() == Catch::Approx(100.0));

    // MoveMechanical goes to a physical angle whatever the offset.
    fake.set_rotator_polls(0);
    driver->move_mechanical(90.0);
    settle(*driver, clock);
    CHECK(driver->get_mechanical_position() == Catch::Approx(90.0));
    CHECK(driver->get_position() == Catch::Approx(155.0));
}

TEST_CASE("iOptron iFR Rotator Driver - Reverse mirrors Position, not the mechanical angle",
          "[ioptron][ifr][rotator][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    fake.set_rotator_polls(0);
    auto driver = connected_rotator(fake, clock);

    driver->set_reverse(true);
    CHECK(fake.count(":RR0#") == 1);
    CHECK(driver->get_reverse() == true);
    CHECK(driver->get_position() == Catch::Approx(325.0));
    CHECK(driver->get_mechanical_position() == Catch::Approx(35.0));

    SECTION("Sync while reversed lands on the requested Position") {
        // The firmware's own :RY is wrong while reversed, so the driver syncs in the normal frame.
        driver->sync(100.0);
        const auto cmds = fake.commands();
        // ... :RR1#, :RY<mirror of 100 = 260>#, :RR0#
        REQUIRE(cmds.size() >= 3);
        CHECK(cmds[cmds.size() - 3] == ":RR1#");
        CHECK(cmds[cmds.size() - 2] == ":RY093600000#");
        CHECK(cmds[cmds.size() - 1] == ":RR0#");
        next_poll(clock);
        CHECK(driver->get_reverse() == true);
        CHECK(driver->get_position() == Catch::Approx(100.0));
        CHECK(driver->get_mechanical_position() == Catch::Approx(35.0));
    }

    SECTION("MoveMechanical while reversed goes to the physical angle") {
        driver->move_mechanical(90.0);
        settle(*driver, clock);
        CHECK(driver->get_mechanical_position() == Catch::Approx(90.0));
        CHECK(driver->get_position() == Catch::Approx(270.0));
    }

    SECTION("Reverse back restores the normal frame") {
        driver->set_reverse(false);
        CHECK(driver->get_position() == Catch::Approx(35.0));
    }
}

TEST_CASE("iOptron iFR Rotator Driver - a unit that stops answering faults, then loses the link",
          "[ioptron][ifr][rotator][link-lost]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto rotator = connected_rotator(fake, clock);
    auto focuser = ifr::create_ifr_focuser(0, fake.slave_path(), "ifr3", clock);
    focuser->set_connected(true);

    // Decision 0009 rule 1 on the rotator's own :RI# reads.
    fake.set_silent(true);
    for (int i = 0; i < 3; ++i) {
        next_poll(clock);
        require_alpaca_error([&] { (void)rotator->get_position(); }, AlpacaError::DriverException);
    }
    CHECK(rotator->get_connected());
    CHECK_FALSE(rotator->get_link_fault().empty());
    // Every write is refused while the fault stands: none may read as done on a silent ack.
    require_alpaca_error([&] { rotator->move_absolute(90.0); }, AlpacaError::DriverException);
    require_alpaca_error([&] { rotator->set_reverse(true); }, AlpacaError::DriverException);

    // Rule 2: past the staleness bound the next refused write loses the link for both devices.
    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));
    require_alpaca_error([&] { rotator->move_absolute(90.0); }, AlpacaError::NotConnected);
    CHECK_FALSE(rotator->get_connected());
    CHECK_FALSE(focuser->get_connected());
    CHECK(rotator->get_link_fault() == focuser->get_link_fault());
    rotator->set_connected(false);
    focuser->set_connected(false);
}
