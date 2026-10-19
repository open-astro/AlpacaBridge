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
#include <alpacacore/vendor/touptek/touptek_powerbox_wrapper.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <functional>
#include <string>

#include "catch2_compat.h"
#include "fake_touptek_powerbox_sdk.h"
#include "locked_touptek_powerbox_sdk.h"

// The StellaVita power-box seam (issue #1326): the switch driver runs over a scripted fake,
// directly and behind the Locked decorator that serialises every call.

namespace {

namespace touptek = alpacacore::vendor::touptek;
using alpacacore::AlpacaException;
using alpacacore::test::FakeTouptekPowerboxSDK;
using alpacacore::test::LockedTouptekPowerboxSDK;

int error_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    }
    return -1;
}

touptek::TouptekSwitchConfig mixed_config() {
    auto cfg = touptek::default_stellavita_config();
    cfg.ports[1].pwm_enabled = true;
    cfg.ports[3].writable = false;
    return cfg;
}

}  // namespace

// Falsified by: touptek_powerbox_wrapper.cpp Impl constructor storing 0 instead of 1 for a boolean
// port (a fresh wrapper reads off, so a connect would cut power).
TEST_CASE("ToupTek powerbox seam - the real wrapper starts every port on before open()",
          "[touptek][switch][unit][fake-sdk]") {
    const auto cfg = mixed_config();
    touptek::TouptekPowerboxWrapper wrapper(cfg.gpio_chip_path, cfg.ports, cfg.pwm_frequency_hz);
    CHECK_FALSE(wrapper.is_open());
    CHECK(wrapper.get_value(0) == 1);
    CHECK(wrapper.get_value(1) == 100);  // PWM port: full duty
    CHECK(wrapper.get_value(2) == 1);
    CHECK(wrapper.get_value(3) == 1);
    CHECK(wrapper.port_config(1).pwm_enabled);
    CHECK(wrapper.gpio_chip_path() == "/dev/gpiochip0");
}

// Falsified by: fake_touptek_powerbox_sdk.h set_value() skipping the writable check (a read-only
// port accepts a write).
TEST_CASE("FakeTouptekPowerboxSDK - set_value enforces the real wrapper's rules", "[touptek][switch][unit][fake-sdk]") {
    const auto cfg = mixed_config();
    FakeTouptekPowerboxSDK fake(cfg.ports);
    CHECK(error_code([&] { fake.set_value(0, 0); }) == alpacacore::AlpacaError::NotConnected);
    fake.open();
    REQUIRE(fake.is_open());
    CHECK(error_code([&] { fake.set_value(3, 0); }) == alpacacore::AlpacaError::NotImplemented);
    CHECK(error_code([&] { fake.set_value(0, 2); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code([&] { fake.set_value(1, 101); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code([&] { fake.set_value(9, 0); }) == alpacacore::AlpacaError::InvalidValue);
    fake.set_value(1, 40);
    fake.set_value(0, 0);
    CHECK(fake.get_value(1) == 40);
    CHECK(fake.get_value(0) == 0);
    // A reconnect keeps the cached levels, as the real wrapper does.
    fake.close();
    fake.open();
    CHECK(fake.get_value(1) == 40);
    CHECK(fake.get_value(0) == 0);
}

// Falsified by: fake_touptek_powerbox_sdk.h open() ignoring fail_open (the connect succeeds).
TEST_CASE("FakeTouptekPowerboxSDK - fail_open refuses the connect and counts the attempt",
          "[touptek][switch][unit][fake-sdk]") {
    FakeTouptekPowerboxSDK fake;
    fake.fail_open = true;
    auto driver = touptek::create_touptek_switch(0, touptek::default_stellavita_config(), fake);
    CHECK(error_code([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    CHECK(fake.open_calls() == 1);
}

// Falsified by: locked_touptek_powerbox_sdk.h set_value() forwarding index and value swapped (the
// inner object sees the wrong call). The same check breaks each other forward one at a time.
TEST_CASE("FakeTouptekPowerboxSDK behind LockedTouptekPowerboxSDK - forwards every call",
          "[touptek][switch][unit][fake-sdk]") {
    const auto cfg = mixed_config();
    FakeTouptekPowerboxSDK fake(cfg.ports, "/dev/gpiochip7", 250);
    LockedTouptekPowerboxSDK locked(fake);

    CHECK_FALSE(locked.is_open());
    locked.open();
    CHECK(locked.is_open());
    CHECK(locked.port_count() == 4);
    CHECK(locked.port_config(1).name == "Port 2");
    CHECK(locked.port_config(1).pwm_enabled);
    CHECK(locked.get_value(2) == 1);
    locked.set_value(2, 0);
    CHECK(fake.get_value(2) == 0);
    CHECK(locked.get_value(2) == 0);
    CHECK(locked.gpio_chip_path() == "/dev/gpiochip7");
    CHECK(locked.pwm_frequency_hz() == 250);
    locked.close();
    CHECK_FALSE(fake.is_open());

    CHECK(fake.open_calls() == 1);
    CHECK(fake.close_calls() == 1);
    CHECK(fake.is_open_calls() == 3);
    CHECK(fake.port_count_calls() == 1);
    CHECK(fake.port_config_calls() == 2);
    CHECK(fake.set_value_calls() == 1);
    CHECK(fake.gpio_chip_path_calls() == 1);
    CHECK(fake.pwm_frequency_hz_calls() == 1);
    // get_value: one through the locked forward, then fake.get_value(2), then the locked one again.
    CHECK(fake.get_value_calls() == 3);
}

// Falsified by: touptek_switch_driver.cpp set_connected() calling set_value() on every port after
// open (a connect that drives the lines).
TEST_CASE("ToupTek powerbox seam - connecting leaves every port on and writes nothing",
          "[touptek][switch][unit][fake-sdk]") {
    FakeTouptekPowerboxSDK fake;
    auto driver = touptek::create_touptek_switch(0, touptek::default_stellavita_config(), fake);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.open_calls() == 1);
    CHECK(fake.set_value_calls() == 0);
    for (int i = 0; i < 4; ++i) CHECK(driver->get_switch(i));
    // Only the four configured ports are exposed; the Cypress hub lines are not.
    CHECK(driver->get_max_switch() == 4);
    CHECK(error_code([&] { driver->get_switch(4); }) == alpacacore::AlpacaError::InvalidValue);

    driver->set_switch(2, false);
    CHECK_FALSE(driver->get_switch(2));
    CHECK(fake.get_value(2) == 0);
    driver->set_connected(false);
    CHECK(fake.close_calls() == 1);
    CHECK(error_code([&] { driver->get_switch(0); }) == alpacacore::AlpacaError::NotConnected);
}

// Falsified by: touptek_switch_driver.cpp set_switch_value() dropping the PWM range check (101
// reaches the injected power box).
TEST_CASE("ToupTek powerbox seam - PWM and read-only ports map through the injected box",
          "[touptek][switch][unit][fake-sdk]") {
    const auto cfg = mixed_config();
    FakeTouptekPowerboxSDK fake(cfg.ports);
    auto driver = touptek::create_touptek_switch(0, cfg, fake);
    driver->set_connected(true);
    CHECK(driver->get_switch_value(1) == Catch::Approx(100.0));
    driver->set_switch_value(1, 35.0);
    CHECK(fake.get_value(1) == 35);
    const int writes = fake.set_value_calls();
    CHECK(error_code([&] { driver->set_switch_value(1, 101.0); }) == alpacacore::AlpacaError::InvalidValue);
    // The driver refuses it before the box is touched.
    CHECK(fake.set_value_calls() == writes);
    CHECK(error_code([&] { driver->set_switch(3, false); }) == alpacacore::AlpacaError::NotImplemented);
}

// Falsified by: touptek_switch_driver.cpp injected constructor dropping the port_count() comparison
// (a mismatched box is accepted and indexed past its table).
TEST_CASE("ToupTek powerbox seam - a box whose port count differs from the config is refused",
          "[touptek][switch][unit][fake-sdk]") {
    auto cfg = touptek::default_stellavita_config();
    FakeTouptekPowerboxSDK fake(std::vector<touptek::StellaVitaPortConfig>(cfg.ports.begin(), cfg.ports.begin() + 2));
    CHECK(error_code([&] { touptek::create_touptek_switch(0, cfg, fake); }) == alpacacore::AlpacaError::InvalidValue);
}
