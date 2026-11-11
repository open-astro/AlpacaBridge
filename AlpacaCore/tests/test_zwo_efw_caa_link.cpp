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

// ZWO EFW filter wheel and CAA rotator after a USB replug (issue #1668, decision 0009): the EAF shape of
// #1500 and #1666 over the shared open table. A removal code is a lost link at once, a Connect rebinds by
// serial, and a status fault standing past util::kLinkStalenessBound is a lost link found by the next read.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"
#include "fake_zwo_caa_sdk.h"
#include "fake_zwo_efw_sdk.h"

namespace {

using alpacacore::AlpacaError::DriverException;
using alpacacore::AlpacaError::NotConnected;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeZWOCAASDK;
using alpacacore::test::FakeZWOEFWSDK;
using alpacacore::test::require_alpaca_error;

constexpr auto kPastTtl = std::chrono::milliseconds(150);

// Every WARN logged while it lives; restores the previous sink on any exit.
struct WarnCapture {
    std::vector<std::string> messages;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) messages.emplace_back(message);
            });
    }
    ~WarnCapture() { alpacacore::logging::set_log_sink(previous); }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
};

std::size_t lost_lines(const WarnCapture& log, const std::string& serial) {
    std::size_t count = 0;
    for (const auto& message : log.messages) {
        if (message.find(" lost") != std::string::npos && message.find(serial) != std::string::npos) ++count;
    }
    return count;
}

// A connected wheel on a fake clock.
struct Wheel {
    FakeTaskClock clock;  // first: it and the SDK outlive the driver
    FakeZWOEFWSDK sdk;
    std::unique_ptr<alpacacore::FilterWheelDriver> driver;

    Wheel() {
        driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk, clock);
        driver->set_connected(true);
    }
    // Three failed Position reads, one TTL apart: the cache latches its fault.
    void latch() {
        sdk.fail_reads = true;
        for (int i = 0; i < 3; ++i) {
            clock.advance(kPastTtl);
            require_alpaca_error([&] { (void)driver->get_position(); }, DriverException);
        }
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }
};

// A connected rotator on a fake clock.
struct Rotator {
    FakeTaskClock clock;  // first: it and the SDK outlive the driver
    FakeZWOCAASDK sdk;
    std::unique_ptr<alpacacore::RotatorDriver> driver;

    Rotator() {
        driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk, clock);
        driver->set_connected(true);
    }
    void latch() {
        sdk.fail_reads = true;
        for (int i = 0; i < 3; ++i) {
            clock.advance(kPastTtl);
            require_alpaca_error([&] { (void)driver->get_position(); }, DriverException);
        }
        REQUIRE_FALSE(driver->get_link_fault().empty());
    }
};

}  // namespace

// ---- EFW filter wheel ----

// Falsified by: zwo_filterwheel_driver.cpp call_locked() rethrowing a NotConnected from the SDK without
// link_.mark_lost() (Connected stays true).
TEST_CASE("ZWO EFW link - a wheel removed from USB reads Connected false at once", "[zwo][filterwheel][unit][link]") {
    WarnCapture log;
    Wheel rig;
    CHECK(rig.driver->get_position() == 0);

    rig.sdk.remove();
    rig.clock.advance(kPastTtl);
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);

    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.driver->get_link_fault().find("ZWO EFW EFW-SN-1 was removed from USB") != std::string::npos);
    require_alpaca_error([&] { rig.driver->set_position(1); }, NotConnected);
    CHECK(rig.sdk.holders(5) == 0);  // the table closed the dead handle
    CHECK(lost_lines(log, "EFW-SN-1") == 1);

    rig.driver->set_connected(false);  // a lost wheel's Disconnect completes and clears the reason (rule 5)
    CHECK(rig.driver->get_link_fault().empty());
}

// Falsified by: zwo_filterwheel_driver.cpp bind_wheel_locked() opening resolve_wheel_id() on every Connect (the
// configured id 5, gone after the replug).
TEST_CASE("ZWO EFW link - a replugged wheel is reopened by one Connect under its new id",
          "[zwo][filterwheel][unit][link]") {
    Wheel rig;
    rig.sdk.remove();
    rig.clock.advance(kPastTtl);
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);
    rig.sdk.replug(9);

    rig.driver->set_connected(true);  // Connect over the lost link: no Disconnect needed (rule 4)
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.sdk.last_open_id == 9);
    rig.clock.advance(kPastTtl);
    CHECK(rig.driver->get_position() == 0);
    CHECK(rig.sdk.last_read_id == 9);
    rig.driver->set_position(2);
    CHECK(rig.sdk.last_move_id == 9);
}

// Falsified by: zwo_filterwheel_driver.cpp bind_wheel_locked() keeping the first candidate it opened instead of
// the one whose serial matches.
TEST_CASE("ZWO EFW link - two wheels swapped while disconnected are told apart by serial",
          "[zwo][filterwheel][unit][link]") {
    FakeTaskClock clock;
    FakeZWOEFWSDK sdk;
    sdk.second_id = 6;
    auto driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.id, sdk, clock);
    driver->set_connected(true);
    driver->set_connected(false);

    // The wheels come back with their ids swapped: EFW-SN-2 on 5 (enumerated first), EFW-SN-1 on 6.
    sdk.remove();
    sdk.second_id = 5;
    sdk.replug(6);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(sdk.holders(6) == 1);
    CHECK(sdk.holders(5) == 0);
    clock.advance(kPastTtl);
    (void)driver->get_position();
    CHECK(sdk.last_read_id == 6);
}

// Falsified by: zwo_filterwheel_driver.cpp get_position() without lose_link_if_fault_stale_locked() in its catch
// (the fault stands for ever with Connected true).
TEST_CASE("ZWO EFW link - a Position fault standing past the staleness bound loses the link",
          "[zwo][filterwheel][unit][link]") {
    Wheel rig;
    rig.latch();

    rig.clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, DriverException);
    CHECK(rig.driver->get_connected());  // inside the bound: a fault (rule 1)

    rig.clock.advance(std::chrono::seconds(2));
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().find("did not answer for 30 s") != std::string::npos);
    CHECK(rig.sdk.holders(5) == 0);  // the wheel may still be attached: the driver closed its open
    CHECK(rig.sdk.open_calls == 1);  // nothing reopened it (rule 4)

    rig.sdk.fail_reads = false;
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_position() == 0);
}

// Falsified by: zwo_filterwheel_driver.cpp set_position() without the latched-fault refusal (the move reaches the
// SDK).
TEST_CASE("ZWO EFW link - a latched Position fault refuses a move", "[zwo][filterwheel][unit][link]") {
    Wheel rig;
    rig.latch();
    const int moves = rig.sdk.set_position_calls;

    require_alpaca_error([&] { rig.driver->set_position(3); }, DriverException);
    CHECK(rig.sdk.set_position_calls == moves);
    CHECK(rig.driver->get_connected());
}

// ---- CAA rotator ----

// Falsified by: zwo_rotator_driver.cpp call_locked() rethrowing a NotConnected from the SDK without
// link_.mark_lost() (Connected stays true).
TEST_CASE("ZWO CAA link - a rotator removed from USB reads Connected false at once", "[zwo][rotator][unit][link]") {
    WarnCapture log;
    Rotator rig;
    CHECK(rig.driver->get_position() == Catch::Approx(0.0));

    rig.sdk.remove();
    require_alpaca_error([&] { rig.driver->move_absolute(90.0); }, NotConnected);  // a move meets it first

    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.driver->get_link_fault().find("ZWO CAA CAA-SN-1 was removed from USB") != std::string::npos);
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);
    require_alpaca_error([&] { rig.driver->halt(); }, NotConnected);
    CHECK(rig.sdk.holders(7) == 0);
    CHECK(lost_lines(log, "CAA-SN-1") == 1);

    rig.driver->set_connected(false);
    CHECK(rig.driver->get_link_fault().empty());
}

// Falsified by: zwo_rotator_driver.cpp bind_rotator_locked() opening resolve_rotator_id() on every Connect (the
// configured id 7, gone after the replug).
TEST_CASE("ZWO CAA link - a replugged rotator is reopened by one Connect under its new id",
          "[zwo][rotator][unit][link]") {
    Rotator rig;
    rig.sdk.remove();
    rig.clock.advance(kPastTtl);
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);
    rig.sdk.replug(11);

    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.sdk.last_open_id == 11);
    CHECK(rig.sdk.move_calls == 0);  // the Connect moved nothing
    rig.clock.advance(kPastTtl);
    (void)rig.driver->get_position();
    CHECK(rig.sdk.last_read_id == 11);
    rig.driver->move_absolute(45.0);
    CHECK(rig.sdk.last_move_id == 11);
}

// Falsified by: zwo_rotator_driver.cpp bind_rotator_locked() keeping the first candidate it opened instead of the
// one whose serial matches.
TEST_CASE("ZWO CAA link - two rotators swapped while disconnected are told apart by serial",
          "[zwo][rotator][unit][link]") {
    FakeTaskClock clock;
    FakeZWOCAASDK sdk;
    sdk.second_id = 8;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk, clock);
    driver->set_connected(true);
    driver->set_connected(false);

    // The rotators come back with their ids swapped: CAA-SN-2 on 7 (enumerated first), CAA-SN-1 on 8.
    sdk.remove();
    sdk.second_id = 7;
    sdk.replug(8);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(sdk.holders(8) == 1);
    CHECK(sdk.holders(7) == 0);
    clock.advance(kPastTtl);
    (void)driver->get_position();
    CHECK(sdk.last_read_id == 8);
}

// Falsified by: zwo_rotator_driver.cpp read_status() without lose_link_if_fault_stale_locked() in its catch (the
// fault stands for ever with Connected true).
TEST_CASE("ZWO CAA link - a status fault standing past the staleness bound loses the link",
          "[zwo][rotator][unit][link]") {
    Rotator rig;
    rig.latch();

    rig.clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, DriverException);
    CHECK(rig.driver->get_connected());

    rig.clock.advance(std::chrono::seconds(2));
    require_alpaca_error([&] { (void)rig.driver->get_position(); }, NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().find("did not answer for 30 s") != std::string::npos);
    CHECK(rig.sdk.holders(7) == 0);
    CHECK(rig.sdk.open_calls == 1);

    rig.sdk.fail_reads = false;
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.driver->get_position() == Catch::Approx(0.0));
}

// Falsified by: zwo_rotator_driver.cpp move_absolute() without refuse_if_faulted() (the move reaches the SDK).
TEST_CASE("ZWO CAA link - a latched status fault refuses a move but not a Halt", "[zwo][rotator][unit][link]") {
    Rotator rig;
    rig.latch();
    const int moves = rig.sdk.move_calls;

    require_alpaca_error([&] { rig.driver->move_absolute(30.0); }, DriverException);
    require_alpaca_error([&] { rig.driver->move_mechanical(30.0); }, DriverException);
    CHECK(rig.sdk.move_calls == moves);
    CHECK_NOTHROW(rig.driver->halt());
    CHECK(rig.driver->get_connected());
}

// Falsified by: zwo_rotator_driver.cpp set_reverse() without refuse_if_faulted() (the Reverse write reaches the SDK).
TEST_CASE("ZWO CAA link - a latched status fault refuses a Reverse write", "[zwo][rotator][unit][link]") {
    Rotator rig;
    rig.latch();

    require_alpaca_error([&] { rig.driver->set_reverse(true); }, DriverException);
    CHECK_FALSE(rig.sdk.reverse);  // the write never reached the rotator
    CHECK(rig.driver->get_connected());
}

// Falsified by: zwo_rotator_driver.cpp bind_rotator_locked() publishing rotator_id_ and rotator_info_ when the scan
// matches the serial, before publish_connect_state() (the failed reconnect leaves the scanned rotator's name).
TEST_CASE("ZWO CAA link - a reconnect that fails after the serial scan publishes no identity",
          "[zwo][rotator][unit][link]") {
    FakeTaskClock clock;
    FakeZWOCAASDK sdk;
    sdk.name = "CAA M54";  // apart from the "ZWO CAA" fallback, so a kept identity shows
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0, sdk, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_name() == "ZWO CAA M54");
    driver->set_connected(false);  // an index binding forgets its id and info
    REQUIRE(driver->get_name() == "ZWO CAA");

    sdk.fail_info = true;  // the scan finds the serial, then the connect's info read fails
    CHECK_THROWS(driver->set_connected(true));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "ZWO CAA");
    CHECK(sdk.holders(sdk.id) == 0);  // the failed connect closed what it opened
}

// Falsified by: zwo_rotator_driver.cpp resolve_rotator_id() publishing rotator_id_ and rotator_info_ for the slot it
// found, before publish_connect_state() (the failed first connect leaves the slot's name).
TEST_CASE("ZWO CAA link - a first connect that fails after resolving the index publishes no identity",
          "[zwo][rotator][unit][link]") {
    FakeTaskClock clock;
    FakeZWOCAASDK sdk;
    sdk.name = "CAA M54";  // apart from the "ZWO CAA" fallback, so a published identity shows
    sdk.fail_info = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(0, 0, sdk, clock);

    CHECK_THROWS(driver->set_connected(true));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "ZWO CAA");
    CHECK(sdk.holders(sdk.id) == 0);
}
