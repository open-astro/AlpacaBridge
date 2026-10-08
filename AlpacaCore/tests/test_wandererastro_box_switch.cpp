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
#include <alpacacore/vendor/wandererastro/wandererastro_box_protocol_wrapper.h>
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/version.h>

#include <functional>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("WandererAstro Box Switch Driver - Defaults", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "WandererAstro WandererBox Pro V3");
    CHECK(driver->get_max_switch() == 24);
    CHECK_FALSE(driver->get_connecting());
}

TEST_CASE("WandererAstro Box Switch Driver - Device metadata", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(3, "/dev/null");

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "WandererAstro WandererBox Pro V3 Power Box Switch Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore WandererAstro WandererBox Switch Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "WANDERERASTRO_BOX_3");
    // Firmware surfaces via the web UI only, and only once connected.
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("WandererAstro Box Switch Driver - Not connected throws", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(2, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(4, 128.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async(2, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(4, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_name(0, "x"); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_description(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_step(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Box Switch Driver - Unsupported actions", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("WandererAstro Box Switch Driver - Value range validation", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(24, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(-1, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(24); }, alpacacore::AlpacaError::InvalidValue);
    // Position value validation itself runs after the connection check, so a
    // bad value on a disconnected driver reports NotConnected — same ordering
    // as the other Wanderer switch drivers.
    require_alpaca_error([&]() { driver->set_switch_value(4, 500.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("WandererAstro Box Switch Driver - Disconnected DeviceState", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/null");

    // DeviceState is the empty list while disconnected: the SwitchDriver base
    // builds it from the public getters, which throw NotConnected and are
    // omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
    const auto state = driver->get_device_state();
    REQUIRE(state.empty());
}

TEST_CASE("WandererAstro Box Switch Driver - Connect failure on invalid port", "[wandererastro][switch][unit]") {
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, "/dev/nonexistent-box-port");

    // Synchronous connect against a missing port must fail with NotConnected
    // and leave the driver disconnected.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // set_connected(false) while already disconnected is an idempotent no-op.
    CHECK_NOTHROW(driver->set_connected(false));
}

TEST_CASE("WandererAstro Box Protocol Wrapper - Defaults and disconnected state", "[wandererastro][switch][unit]") {
    using namespace alpacacore::vendor::wandererastro;

    // Hardware constants the Switch surface is built on.
    static_assert(kBoxPwmMax == 255);
    CHECK(kBoxDc34VoltageMin == 5.0);
    CHECK(kBoxDc34VoltageMax == 13.2);
    CHECK(kBoxDc34VoltageStep == 0.1);
    CHECK(kBoxCalibratedPowerMinFirmware == 20240216);

    WandererBoxProtocolWrapper wrapper;
    CHECK_FALSE(wrapper.is_connected());
    CHECK_FALSE(wrapper.get_firmware_date().has_value());

    const auto state = wrapper.get_state();
    CHECK_FALSE(state.valid);
    CHECK(state.input_voltage == 0.0);
    CHECK_FALSE(state.dc3_4);

    // Operations on a disconnected wrapper report NotConnected; out-of-range
    // arguments report InvalidValue regardless of connection.
    require_alpaca_error([&]() { wrapper.set_dc3_4(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_pwm(5, 128); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_pwm(4, 128); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_pwm(5, 256); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dc3_4_voltage(4.9); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dc3_4_voltage(13.3); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_usb(5, true); }, alpacacore::AlpacaError::InvalidValue);

    // The protocol baud is fixed: a non-19200 config is rejected as
    // InvalidValue before any port is opened.
    BoxConnectionConfig bad_baud;
    bad_baud.serial_port = "/dev/null";
    bad_baud.baud_rate = 9600;
    require_alpaca_error([&]() { wrapper.connect(bad_baud); }, alpacacore::AlpacaError::InvalidValue);

    // Disconnect on a never-connected wrapper is a safe no-op.
    CHECK_NOTHROW(wrapper.disconnect());
}

// ---------------------------------------------------------------------------
// Issue #237: a device that stops streaming must not be served from the cache
// forever. Wire-level over a pty-backed streamer (fake_serial_streamer.h).
// ---------------------------------------------------------------------------

#include <chrono>
#include <thread>

#include "fake_serial_streamer.h"
#include "fake_task_clock.h"

namespace {

// Moves the fake clock by @p d, then waits (bounded, real time) for the
// reader thread's next pass: its silence check is the one clock read a muted
// or severed link makes per pass, taken under the lock the driver's getters
// also take, so a getter called after this returns sees that pass's verdict.
bool advance_one_pass(alpacacore::test::FakeTaskClock& clock, std::chrono::nanoseconds d) {
    clock.advance(d);
    return clock.wait_for_now_calls(clock.now_calls() + 1, std::chrono::milliseconds(2000));
}

// set_muted() can land just after the streamer committed one more frame to the
// pty; give the reader time to take it at the current virtual time before the
// clock moves, or it would restart the silence window.
void drain_after_mute() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

template <typename Pred>
bool wait_until_box(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

}  // namespace

// 22 'A'-terminated fields after the identity token, in the wire order the
// wrapper indexes: fw, probe1-3, humidity, ambient, total A, 19 V A, DC3-4 A,
// input V, USB3.1 x3, USB2 x2, DC3-4, DC5/6/7 PWM, DC8-9, DC10-11, DC3-4 set x10.
const char* const kBoxFrame =
    "ZXWBProV3A20250410A-127.00A-127.00A-127.00A45.20A21.30A1.50A0.20A0.30A13.10A1A1A1A1A1A1A0A0A0A1A1A120A\n";

TEST_CASE("WandererAstro Box Switch Driver - Silent link faults reads, frames restore them (issue #237)",
          "[wandererastro][switch][unit][fake]") {
    alpacacore::test::FakeTaskClock clock;  // outlives the driver
    alpacacore::test::FakeSerialStreamer box(kBoxFrame, std::chrono::milliseconds(500));
    auto driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, box.slave_path(), 19200, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_value(14) == 13.1);  // input voltage, live from the stream
    driver->set_switch(7, true);                  // DC8-9: a commanded value that must not survive the fault
    CHECK(driver->get_switch(7));

    box.set_muted(true);
    auto value_read_throws = [&] {
        try {
            (void)driver->get_switch_value(14);
            return false;
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
    };
    drain_after_mute();
    // Reads keep answering from cache through exactly 10 s of silence (the
    // limit is inclusive); one millisecond more latches the fault.
    REQUIRE(advance_one_pass(clock, std::chrono::seconds(10)));
    CHECK(driver->get_switch_value(14) == 13.1);
    REQUIRE(advance_one_pass(clock, std::chrono::milliseconds(1)));
    CHECK(value_read_throws());
    CHECK(driver->get_connected());  // the client decides whether to reconnect
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing
    require_alpaca_error([&]() { (void)driver->get_switch_value(14); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_switch(7); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_switch(7, false); }, alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(box.received("200"));  // nothing went on the wire while faulted
    // Static metadata does not depend on the box and keeps answering.
    CHECK(driver->get_can_write(7));
    CHECK_FALSE(driver->get_switch_name(14).empty());
    // DeviceState omits the unavailable members rather than failing outright.
    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    CHECK(state.front().name == "TimeStamp");

    // The next frame restores the link without a reconnect.
    box.set_muted(false);
    CHECK(wait_until_box([&] { return !value_read_throws(); }, std::chrono::milliseconds(3000)));
    CHECK(driver->get_switch_value(14) == 13.1);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());

    // A dead fd (EIO) is silence too, and the reason names the read error.
    box.sever_link();
    // Two passes: the second starts after the first counted one returned, so
    // its reads hit the dead fd and the reason can name the read error.
    REQUIRE(clock.wait_for_now_calls(clock.now_calls() + 2, std::chrono::milliseconds(4000)));
    REQUIRE(advance_one_pass(clock, std::chrono::milliseconds(10001)));
    CHECK(value_read_throws());
    try {
        (void)driver->get_switch_value(14);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Input/output error") != std::string::npos);
    }
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}
