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
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/vendor/zwo/zwo_asiair_protocol_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <variant>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "power_port_test_dir.h"
#include "zwo_asiair_stubs.h"

// The libgpiod calls reach the in-memory chip of zwo_asiair_stubs.h (issues #772, #300) when a case names
// kStubChip; the default /dev/gpiochip0 still reaches the real library.
namespace {

using alpacacore::test::kStubChip;
using alpacacore::test::stub_claimed;
using alpacacore::test::stub_level;
using alpacacore::test::stub_level_at_release;
using alpacacore::test::stub_level_changes;
using alpacacore::test::stub_reset_levels;
using alpacacore::test::stub_set_level;

using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Defaults", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO ASIAIR Pro Switch");
    REQUIRE(driver->get_max_switch() == 4);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Device metadata", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        3, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "ZWO ASIAIR Pro 12V power switch (/dev/gpiochip0)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Pro Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_ASIAIR_3");
}

TEST_CASE("ZWO ASIAIR Plus (Pi CM4) Switch Driver - model label", "[zwo][switch][asiair][unit]") {
    // The Pi CM4 ASIAIR Plus reuses the Pro libgpiod driver but must not
    // identify itself as a Pro — the router sets model_name accordingly.
    auto config = alpacacore::vendor::zwo::default_asiair_pro_config();
    config.model_name = "ASIAIR Plus";
    config.board = "Pi CM4";
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, config);

    REQUIRE(driver != nullptr);
    CHECK(driver->get_name() == "ZWO ASIAIR Plus Switch");
    CHECK(driver->get_description() == "ZWO ASIAIR Plus (Pi CM4) 12V power switch (/dev/gpiochip0)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Plus (Pi CM4) Switch");
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Not connected throws", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    REQUIRE_FALSE(driver->get_connected());

    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(2); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Unsupported actions", "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Per-port metadata", "[zwo][switch][asiair][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.ports[3].pwm_enabled = true; // mark Port 4 (GPIO 18) as a PWM channel
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);

    REQUIRE(driver->get_max_switch() == 4);

    // All four switches are writable, none are async.
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
    }

    // Default port names match the ASIAIR physical labels.
    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Boolean channels have range [0, 1]; the PWM channel exposes [0, 100].
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(3) == Catch::Approx(100.0));

    // Descriptions surface the underlying GPIO line and mode.
    CHECK(driver->get_switch_description(0) == "ASIAIR power port on GPIO 12 (on/off)");
    CHECK(driver->get_switch_description(1) == "ASIAIR power port on GPIO 13 (on/off)");
    CHECK(driver->get_switch_description(2) == "ASIAIR power port on GPIO 26 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR power port on GPIO 18 (PWM 0-100%)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Switch ID range validation",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // Out-of-range IDs are rejected with InvalidValue regardless of connection state.
    require_alpaca_error([&]() { driver->get_switch_name(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); }, alpacacore::AlpacaError::InvalidValue);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(4); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - State machine when disconnected",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // ASCOM contract: a disconnected switch reports get_connected() = false,
    // get_connecting() = false, and get_device_state() returns the empty list.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    {
        // DeviceState is the empty list while disconnected: the SwitchDriver
        // base builds it from the public getters, which throw NotConnected and
        // are omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
        const auto state = driver->get_device_state();
        REQUIRE(state.empty());
    }
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Unsupported method error codes",
          "[zwo][switch][asiair][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(
        0, alpacacore::vendor::zwo::default_asiair_pro_config());

    // Asynchronous mutation is not implemented; ASCOM expects NotImplemented,
    // not a generic DriverException. Connection check fires first because the
    // ASCOM contract requires NotConnected before any other validation.
    require_alpaca_error([&]() { driver->set_async(0, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); }, alpacacore::AlpacaError::NotConnected);

    // Action/Command methods are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - Constructor rejects invalid configs",
          "[zwo][switch][asiair][unit]") {
    // Empty ports list must fail at construction (the wrapper has nothing to manage).
    alpacacore::vendor::zwo::AsiairSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, empty_cfg); },
        alpacacore::AlpacaError::InvalidValue);

    // PWM frequency of 0 Hz is rejected (would divide by zero in the period calc).
    auto zero_freq = alpacacore::vendor::zwo::default_asiair_pro_config();
    zero_freq.pwm_frequency_hz = 0;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, zero_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // Absurdly high PWM frequency is rejected.
    auto huge_freq = alpacacore::vendor::zwo::default_asiair_pro_config();
    huge_freq.pwm_frequency_hz = 1000001;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, huge_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // A GPIO chip path that is not an absolute /dev/ node is rejected.
    for (const char* bad : {"", "gpiochip0", "/sys/class/gpio", "relative/gpiochip0"}) {
        auto bad_chip = alpacacore::vendor::zwo::default_asiair_pro_config();
        bad_chip.gpio_chip_path = bad;
        require_alpaca_error([&]() { alpacacore::vendor::zwo::create_zwo_asiair_switch(0, bad_chip); },
                             alpacacore::AlpacaError::InvalidValue);
    }
}

// Issue #772 (CC-15): close() stops the PWM workers wherever they are in the
// cycle, so without a settle step a port at 50 % duty is released low about
// half the time and the heater on it loses power at disconnect. ascom-contract.instructions.md
// "Never power-cycle on disconnect": duty > 0 is released high, duty 0 low.
TEST_CASE("ZWO ASIAIR Pro protocol wrapper - close releases each PWM line at its steady level",
          "[zwo][switch][asiair][unit][gpio]") {
    constexpr unsigned int kHalfLine = 5;
    constexpr unsigned int kOffLine = 6;
    constexpr int kCloses = 20;
    int half_released_low = 0;
    int off_released_high = 0;
    int not_released = 0;
    for (int i = 0; i < kCloses; ++i) {
        alpacacore::vendor::zwo::AsiairProtocolWrapper wrapper(
            kStubChip, {{"DC1", kHalfLine, true}, {"DC2", kOffLine, true}}, 100);
        wrapper.open();
        wrapper.set_value(0, 50);
        wrapper.set_value(1, 0);
        // Vary where in the 10 ms period close() lands.
        std::this_thread::sleep_for(std::chrono::milliseconds(30 + (i * 7) % 20));
        wrapper.close();
        half_released_low += stub_level_at_release(kHalfLine) == 0 ? 1 : 0;
        off_released_high += stub_level_at_release(kOffLine) == 1 ? 1 : 0;
        not_released += stub_level_at_release(kHalfLine) < 0 ? 1 : 0;
    }
    INFO(half_released_low << "/" << kCloses << " closes released the 50 % port low");
    CHECK(not_released == 0);
    CHECK(half_released_low == 0);
    CHECK(off_released_high == 0);
}

// Issue #300: claiming the lines reads each one and changes none. Before, open()
// requested every line high, so a connect powered on a port the boot left off.
TEST_CASE("ZWO ASIAIR Pro protocol wrapper - open claims each line at its level and changes none",
          "[zwo][switch][asiair][unit][gpio][power-on]") {
    stub_reset_levels(1);
    stub_set_level(13, 0);
    stub_set_level(18, 0);
    alpacacore::vendor::zwo::AsiairProtocolWrapper wrapper(
        kStubChip, {{"Port 1", 12, false}, {"Port 2", 13, false}, {"Port 3", 26, true}, {"Port 4", 18, true}}, 100);
    wrapper.open();
    CHECK(wrapper.get_value(0) == 1);
    CHECK(wrapper.get_value(1) == 0);
    CHECK(wrapper.get_value(2) == 100);  // PWM port, line high: full duty
    CHECK(wrapper.get_value(3) == 0);    // PWM port, line low: 0 %, not 100 %
    CHECK(stub_level_changes() == 0);
    CHECK(stub_level(13) == 0);

    // close() leaves a 40 % port at its steady level (high); a reconnect resumes 40 %.
    wrapper.set_value(2, 40);
    wrapper.close();
    REQUIRE(stub_level(26) == 1);
    wrapper.open();
    CHECK(wrapper.get_value(2) == 40);
    CHECK(wrapper.get_value(1) == 0);
    wrapper.close();
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - off at server start drives every port low before any connect",
          "[zwo][switch][asiair][unit][gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    stub_reset_levels(1);
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.gpio_chip_path = kStubChip;
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);
    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    for (const unsigned int line : {12u, 13u, 26u, 18u}) {
        CHECK(stub_level(line) == 0);
    }
    const int changes = stub_level_changes();
    driver->set_connected(true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_switch(i));
    }
    CHECK(stub_level_changes() == changes);  // the connect took the held lines over unchanged
    driver->set_switch(1, true);
    CHECK(alpacacore::util::PowerPortStateStore::load(driver->get_unique_id())[1] == 100.0);
    driver->set_connected(false);
}

TEST_CASE("ZWO ASIAIR Pro Switch Driver - a Disconnect before any Connect keeps lines a start holds",
          "[zwo][switch][asiair][unit][gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    stub_reset_levels(1);
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.gpio_chip_path = kStubChip;
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);
    driver->apply_power_on_state(true);
    REQUIRE(stub_level(12) == 0);
    // A client's Disconnect with nothing connected must not release them (decision 0021 rule 5):
    // the stub records a level for a line only when its request is released.
    driver->set_connected(false);
    CHECK(stub_level_at_release(12) == -1);
    driver->set_connected(true);
    driver->set_connected(false);
    CHECK(stub_level_at_release(12) == 0);
}

// Issue #1862: every ISwitchV3 member while connected, each value checked against the stub chip. Port 2 boots
// off and Port 4 is a PWM port.
TEST_CASE("ZWO ASIAIR Pro Switch Driver - every ISwitchV3 member answers while connected",
          "[zwo][switch][asiair][unit][gpio]") {
    alpacacore::test::TempPowerPortDir dir;
    stub_reset_levels(1);
    stub_set_level(13, 0);
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.gpio_chip_path = kStubChip;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(stub_claimed());

    REQUIRE(driver->get_max_switch() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_min_switch_value(i) == 0.0);
        CHECK(driver->get_max_switch_value(i) == (i == 3 ? 100.0 : 1.0));
        CHECK(driver->get_switch_step(i) == 1.0);
        CHECK(driver->get_state_change_complete(i));
        CHECK(driver->get_switch_name(i) == "Port " + std::to_string(i + 1));
    }
    CHECK(driver->get_switch_description(1) == "ASIAIR power port on GPIO 13 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR power port on GPIO 18 (PWM 0-100%)");

    // The connect read the lines: Port 2 is off, the others (Port 4 at full duty) on.
    CHECK(driver->get_switch(0));
    CHECK_FALSE(driver->get_switch(1));
    CHECK(driver->get_switch_value(1) == 0.0);
    CHECK(driver->get_switch_value(3) == 100.0);

    // Writes reach the lines and read back.
    driver->set_switch(0, false);
    CHECK(stub_level(12) == 0);
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch_value(0) == 0.0);
    driver->set_switch_value(1, 1.0);
    CHECK(stub_level(13) == 1);
    CHECK(driver->get_switch(1));
    driver->set_switch_value(3, 40.0);
    CHECK(driver->get_switch_value(3) == 40.0);
    CHECK(driver->get_switch(3));

    // Out of range while connected: InvalidValue, and nothing written. The boolean lines are compared, not the
    // global change count: the 40 % PWM worker drives GPIO 18 twice every period throughout this block.
    const int line12 = stub_level(12);
    const int line13 = stub_level(13);
    const int line26 = stub_level(26);
    require_alpaca_error([&] { driver->set_switch_value(0, 2.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(3, 101.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(3, std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch(4, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_name(4, "X"); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(stub_level(12) == line12);
    CHECK(stub_level(13) == line13);
    CHECK(stub_level(26) == line26);
    CHECK(driver->get_switch_value(0) == 0.0);
    CHECK(driver->get_switch_value(3) == 40.0);

    // Async writes are not implemented on a connected driver.
    require_alpaca_error([&] { driver->set_async(0, true); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&] { driver->set_async_value(3, 50.0); }, alpacacore::AlpacaError::NotImplemented);

    // DeviceState carries the three members of every port, then TimeStamp.
    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 13);
    CHECK(state[0].name == "GetSwitch0");
    CHECK(std::get<bool>(state[0].value) == false);
    CHECK(state[10].name == "GetSwitchValue3");
    CHECK(std::get<double>(state[10].value) == 40.0);
    CHECK(state[12].name == "TimeStamp");

    // A name lasts as long as the driver, across a reconnect.
    driver->set_switch_name(2, "Flat Panel");
    CHECK(driver->get_switch_name(2) == "Flat Panel");

    // Disconnect releases every line at its steady level: the boolean ports as commanded, the 40 % port high.
    driver->set_connected(false);
    CHECK_FALSE(stub_claimed());
    CHECK(stub_level_at_release(12) == 0);
    CHECK(stub_level_at_release(13) == 1);
    CHECK(stub_level_at_release(26) == 1);
    CHECK(stub_level_at_release(18) == 1);
    CHECK(driver->get_switch_name(2) == "Flat Panel");

    // A client that turns every port off before it disconnects finds every line released off.
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_name(2) == "Flat Panel");
    CHECK(driver->get_switch_value(3) == 40.0);  // the reconnect reads the 40 % port's held line back as its duty
    for (int i = 0; i < 4; ++i) {
        driver->set_switch(i, false);
    }
    driver->set_connected(false);
    for (const unsigned int line : {12u, 13u, 26u, 18u}) {
        CHECK(stub_level_at_release(line) == 0);
    }
}
