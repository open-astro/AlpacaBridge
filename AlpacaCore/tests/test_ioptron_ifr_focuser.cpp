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

// The iOptron iFR focuser half, and the serial session it shares with the rotator half, over FakeIoptronIfr
// (fake_ioptron_ifr.h). Cases 3 and 4 and the disconnected out-of-range probe of case 6 come from the
// contract sweep entry (contract_sweep.h, ifr_focuser).

#include <alpacacore/focuser_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_rotator_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>

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

// Past the 100 ms status cache, inside the 1 s move hold.
constexpr auto kPastTtl = std::chrono::milliseconds(150);

std::unique_ptr<alpacacore::FocuserDriver> connected_focuser(FakeIoptronIfr& fake, FakeTaskClock& clock,
                                                             const std::string& model = "ifr3") {
    auto driver = ifr::create_ifr_focuser(0, fake.slave_path(), model, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

// The next read is a fresh :FI#.
void next_poll(FakeTaskClock& clock) { clock.advance(kPastTtl); }

}  // namespace

TEST_CASE("iOptron iFR Focuser Driver - Defaults", "[ioptron][ifr][focuser][unit]") {
    auto driver = ifr::create_ifr_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "iOptron iFR 3\" Focuser");
    CHECK(driver->get_absolute() == true);
    CHECK(ifr::create_ifr_focuser(0, "/dev/ttyUSB0", "ifr2")->get_name() == "iOptron iFR 2\" Focuser");
    CHECK(ifr::create_ifr_focuser(0, "/dev/ttyUSB0", "ifr4")->get_name() == "iOptron iFR 4\" Focuser");
    // An unknown model reads as the 3", the default.
    CHECK(ifr::create_ifr_focuser(0, "/dev/ttyUSB0", "ifr9")->get_name() == "iOptron iFR 3\" Focuser");
}

TEST_CASE("iOptron iFR Focuser Driver - Device metadata", "[ioptron][ifr][focuser][unit]") {
    auto driver = ifr::create_ifr_focuser(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "iOptron iFR 3\" focuser-rotator, focuser");
    CHECK(driver->get_driver_info() == "AlpacaCore iOptron iFR Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "IOPTRON_IFR_FOCUSER_3");
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("iOptron iFR Focuser Driver - Device specifics once connected", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_focuser(fake, clock);

    CHECK(driver->get_max_step() == 62500);
    CHECK(driver->get_max_increment() == 62500);
    CHECK(driver->get_step_size() == Catch::Approx(0.4));
    CHECK(driver->get_temperature() == Catch::Approx(298.58 - 273.15).epsilon(1e-6));
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("260324"));

    // DeviceState: the focuser base class builds it from the getters.
    const auto state = driver->get_device_state();
    bool has_position = false;
    for (const auto& s : state) has_position = has_position || s.name == "Position";
    CHECK(has_position);

    // The 4" travels 26 mm.
    FakeIoptronIfr four;
    FakeTaskClock clock4;
    four.set_rotator_size(40);
    auto big = connected_focuser(four, clock4, "ifr4");
    CHECK(big->get_max_step() == 65000);
    CHECK(big->get_name() == "iOptron iFR 4\" Focuser");
}

// ConformU moves below 0 and above MaxStep and expects the focuser at that limit (issue #2028).
TEST_CASE("iOptron iFR Focuser Driver - A move outside 0..MaxStep goes to that limit",
          "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    fake.set_focuser_position(1000);
    auto driver = connected_focuser(fake, clock);

    driver->move(-1);
    CHECK(fake.count(":FM0000000#") == 1);  // the limit, not the request
    driver->move(62501);
    CHECK(fake.count(":FM0062500#") == 1);
    CHECK(fake.count(":FM") == 2);

    // A position is never refused: the connection check is the first thing a Move does.
    auto offline = ifr::create_ifr_focuser(0, "/dev/ttyUSB0");
    require_alpaca_error([&] { offline->move(62501); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { offline->move(100); }, AlpacaError::NotConnected);
}

TEST_CASE("iOptron iFR Focuser Driver - State machine", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    fake.set_focuser_polls(2);
    auto driver = connected_focuser(fake, clock);

    CHECK(driver->get_is_moving() == false);  // idle after connect

    driver->move(3000);
    CHECK(fake.count(":FM0003000#") == 1);
    // Moving on the first poll after the move, part of the way.
    CHECK(driver->get_is_moving() == true);
    next_poll(clock);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == 3000);

    SECTION("the hold keeps IsMoving true until the motor starts") {
        fake.set_start_delay_polls(2);
        driver->move(4000);
        // The frame still reads stopped at 3000: the motor has not started, the hold answers.
        CHECK(driver->get_is_moving() == true);
        CHECK(driver->get_position() == 3000);
        next_poll(clock);  // the delay is over: running, and the frame is the truth from here
        CHECK(driver->get_is_moving() == true);
        next_poll(clock);
        CHECK(driver->get_is_moving() == true);
        next_poll(clock);
        CHECK(driver->get_is_moving() == false);
        CHECK(driver->get_position() == 4000);
    }

    SECTION("the hold ends at its deadline when nothing ever moves") {
        fake.set_start_delay_polls(1 << 20);
        driver->move(4000);
        CHECK(driver->get_is_moving() == true);
        clock.advance(std::chrono::milliseconds(1100));
        CHECK(driver->get_is_moving() == false);
    }

    SECTION("Halt ends the hold and stops the motor") {
        fake.set_start_delay_polls(1 << 20);
        driver->move(4000);
        driver->halt();
        CHECK(fake.count(":FQ#") == 1);
        CHECK(driver->get_is_moving() == false);
    }
}

TEST_CASE("iOptron iFR Focuser Driver - Unsupported members", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto driver = connected_focuser(fake, clock);

    // No temperature compensation in the hardware: a property it lacks, not a failure.
    require_alpaca_error([&] { driver->set_temp_comp(true); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_temp_comp(false); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->action("SetZero", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { driver->command_blind(":FZ#", true); }, AlpacaError::MethodNotImplemented);
    CHECK(fake.count(":FZ#") == 0);
}

TEST_CASE("iOptron iFR Focuser Driver - a refused move", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    fake.set_focuser_polls(0);
    auto driver = connected_focuser(fake, clock);

    SECTION("to the current position is a Move that has nothing to do") {
        // The firmware answers '0' for the position it is already at (rig, 2026-11-03).
        driver->move(1000);
        CHECK(fake.count(":FM0001000#") == 1);
        CHECK(driver->get_is_moving() == false);
        CHECK(driver->get_position() == 1000);
    }

    SECTION("toward an end stop it already sits at is an error") {
        fake.set_focuser_limits(1000, 62892);
        require_alpaca_error([&] { driver->move(500); }, AlpacaError::DriverException);
        CHECK(driver->get_is_moving() == false);
        CHECK(driver->get_position() == 1000);
    }
}

TEST_CASE("iOptron iFR Focuser Driver - a unit that is not an iFR is refused", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    fake.set_model(3);  // an iAFS2/3, which the iEAF driver serves
    auto driver = ifr::create_ifr_focuser(0, fake.slave_path());
    try {
        driver->set_connected(true);
        FAIL("Expected the connect to be refused");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::NotConnected);
        CHECK(std::string(e.what()).find("not an iOptron iFR") != std::string::npos);
    }
    CHECK(driver->get_connected() == false);
}

TEST_CASE("iOptron iFR - the focuser and the rotator share one session", "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto focuser = ifr::create_ifr_focuser(0, fake.slave_path(), "ifr3", clock);
    auto rotator = ifr::create_ifr_rotator(0, fake.slave_path(), "ifr3", clock);

    focuser->set_connected(true);
    rotator->set_connected(true);
    REQUIRE(focuser->get_connected());
    REQUIRE(rotator->get_connected());
    CHECK(fake.handshakes() == 1);  // the rotator joined the open session

    // One device letting go leaves the other working on the same port.
    focuser->set_connected(false);
    CHECK(rotator->get_connected());
    CHECK(rotator->get_position() == Catch::Approx(35.0));

    // Once both have let go the port is closed: the next connect is a fresh session.
    rotator->set_connected(false);
    focuser->set_connected(true);
    CHECK(fake.handshakes() == 2);
    focuser->set_connected(false);
}

TEST_CASE("iOptron iFR - a pulled cable loses the link for both devices", "[ioptron][ifr][focuser][link-lost]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto focuser = ifr::create_ifr_focuser(0, fake.slave_path(), "ifr3", clock);
    auto rotator = ifr::create_ifr_rotator(0, fake.slave_path(), "ifr3", clock);
    focuser->set_connected(true);
    rotator->set_connected(true);

    fake.unplug();
    require_alpaca_error([&] { focuser->move(2000); }, AlpacaError::NotConnected);
    CHECK(focuser->get_connected() == false);
    CHECK(rotator->get_connected() == false);  // the same session
    CHECK_FALSE(focuser->get_link_fault().empty());
    CHECK(focuser->get_link_fault() == rotator->get_link_fault());
    require_alpaca_error([&] { (void)rotator->get_position(); }, AlpacaError::NotConnected);
    CHECK(focuser->get_device_state().empty());
    CHECK_FALSE(focuser->get_device_firmware().has_value());

    // A Disconnect drops the kept reason (decision 0009 rule 5).
    focuser->set_connected(false);
    CHECK(focuser->get_link_fault().empty());
}

TEST_CASE("iOptron iFR - a unit that stops answering faults, then loses the link for both devices",
          "[ioptron][ifr][focuser][link-lost]") {
    FakeIoptronIfr fake;
    FakeTaskClock clock;
    auto focuser = ifr::create_ifr_focuser(0, fake.slave_path(), "ifr3", clock);
    auto rotator = ifr::create_ifr_rotator(0, fake.slave_path(), "ifr3", clock);
    focuser->set_connected(true);
    rotator->set_connected(true);

    // The USB node stays up; the firmware stops replying. Decision 0009 rule 1: three failed reads latch the
    // fault, Connected stays true, and get_link_fault() is the fault's text.
    fake.set_silent(true);
    for (int i = 0; i < 3; ++i) {
        next_poll(clock);
        require_alpaca_error([&] { (void)focuser->get_position(); }, AlpacaError::DriverException);
    }
    CHECK(focuser->get_connected());
    CHECK(rotator->get_connected());
    CHECK_FALSE(focuser->get_link_fault().empty());
    // A latched fault refuses a write as it refuses the read (a silent ack must not read as a move made).
    require_alpaca_error([&] { focuser->move(2000); }, AlpacaError::DriverException);

    // Rule 2: a fault that has stood past the staleness bound loses the link, for both devices of the unit.
    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));
    require_alpaca_error([&] { (void)focuser->get_position(); }, AlpacaError::NotConnected);
    CHECK_FALSE(focuser->get_connected());
    CHECK_FALSE(rotator->get_connected());
    CHECK_FALSE(focuser->get_link_fault().empty());
    CHECK(focuser->get_link_fault() == rotator->get_link_fault());  // rule 5: the one kept text

    // Rule 4: only Connect relinks, with a fresh handshake, once the unit answers again.
    fake.set_silent(false);
    focuser->set_connected(true);
    CHECK(focuser->get_connected());
    CHECK(focuser->get_link_fault().empty());
    CHECK(fake.handshakes() == 2);
    focuser->set_connected(false);
    rotator->set_connected(false);
}

// #2204: an unknown unit keeps the configured travel and reads under a generic name.
// Falsified by: ioptron_ifr_protocol_wrapper.cpp IfrLink open refusing a code is_ifr_model() omits.
TEST_CASE("iOptron iFR Focuser Driver - an unknown model code connects with the configured travel (#2204)",
          "[ioptron][ifr][focuser][unit]") {
    FakeIoptronIfr fake;
    fake.set_model(7);
    auto driver = ifr::create_ifr_focuser(0, fake.slave_path(), "ifr4");
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(driver->get_name() == "iOptron Focuser");
    CHECK(driver->get_max_step() == 65000);
    driver->set_connected(false);
}

// Falsified by: ioptron_ifr_protocol_wrapper.cpp ifr_refuses_model() returning false for a unit with no rotator.
TEST_CASE("iOptron iFR Focuser Driver - a unit with no rotator or another product is refused (#2204)",
          "[ioptron][ifr][focuser][unit]") {
    const int code = GENERATE(2, 3, 4, 98, 99);
    FakeIoptronIfr fake;
    fake.set_model(code);
    auto driver = ifr::create_ifr_focuser(0, fake.slave_path());
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK(driver->get_connected() == false);
}
