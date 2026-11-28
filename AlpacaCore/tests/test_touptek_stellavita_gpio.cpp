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
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <chrono>
#include <map>
#include <memory>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_gpio_line_backend.h"
#include "fake_touptek_powerbox_sdk.h"
#include "power_port_test_dir.h"

// The StellaVita switch over util::GpioLineBackend (decision 0027): the driver runs over FakeGpioLineBackend and
// FakeTaskClock (bundled in FakeTouptekPowerbox), so no case needs a GPIO chip and none sleeps.

namespace {

namespace touptek = alpacacore::vendor::touptek;
using alpacacore::test::error_code_of;
using alpacacore::test::FakeGpioLineBackend;
using Kind = FakeGpioLineBackend::Event::Kind;

// Port 2 (index 1) is PWM and Port 4 (index 3) is read-only, so the backend opens three lines: its line
// index 0 is Port 1 (GPIO 18), 1 is Port 2 (GPIO 10) and 2 is Port 3 (GPIO 17).
touptek::TouptekSwitchConfig mixed_config() {
    auto cfg = touptek::default_stellavita_config();
    cfg.ports[1].pwm_enabled = true;
    cfg.ports[3].writable = false;
    return cfg;
}

// The virtual clock and the fake backend a driver runs over, declared before the driver so they outlive it.
using Rig = alpacacore::test::FakeTouptekPowerbox;

int count_events(const FakeGpioLineBackend& gpio, Kind kind) {
    int n = 0;
    for (const auto& e : gpio.events()) {
        n += e.kind == kind ? 1 : 0;
    }
    return n;
}

}  // namespace

// Falsified by: touptek_switch_driver.cpp stellavita_ports() handing the engine every port as claimed (the
// read-only Port 4 line, GPIO 4, joins the claim) or dropping the chip path.
TEST_CASE("ToupTek StellaVita Switch Driver - the claim covers the writable ports on the configured chip",
          "[touptek][switch][unit][fake-gpio]") {
    Rig rig;
    auto driver = rig.make(mixed_config());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(rig.gpio.opened_path() == "/dev/gpiochip0");
    CHECK(rig.gpio.opened_lines() == std::vector<unsigned int>{18u, 10u, 17u});
    // The read-only port reads on without a line behind it.
    CHECK(driver->get_switch(3));
    driver->set_connected(false);
}

// Connected, async switch control and a valid write to a read-only port answer NotImplemented (disconnected,
// NotConnected comes first: test_touptek_switch.cpp). An out-of-range write answers InvalidValue before either.
TEST_CASE("ToupTek StellaVita Switch Driver - connected async and read-only writes are NotImplemented",
          "[touptek][switch][unit][fake-gpio]") {
    Rig rig;
    auto driver = rig.make(mixed_config());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(error_code_of([&] { driver->set_async(0, true); }) == alpacacore::AlpacaError::NotImplemented);
    CHECK(error_code_of([&] { driver->set_async_value(0, 1.0); }) == alpacacore::AlpacaError::NotImplemented);
    CHECK(error_code_of([&] { driver->set_switch(3, true); }) == alpacacore::AlpacaError::NotImplemented);
    // The range check precedes the writability check (issue #2264): an out-of-range write to the read-only
    // port is InvalidValue, a valid one NotImplemented, and neither reaches the backend.
    CHECK(error_code_of([&] { driver->set_switch_value(3, 2.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { driver->set_switch_value(3, 1.0); }) == alpacacore::AlpacaError::NotImplemented);
    CHECK(rig.gpio.write_count() == 0);
    driver->set_connected(false);
}

// Falsified by: soft_pwm_ports.cpp open() swallowing a backend that refuses the claim (the connect succeeds).
TEST_CASE("ToupTek StellaVita Switch Driver - a backend that refuses the claim refuses the connect",
          "[touptek][switch][unit][fake-gpio]") {
    Rig rig;
    rig.gpio.fail_open();
    auto driver = rig.make(touptek::default_stellavita_config());
    CHECK(error_code_of([&] { driver->set_connected(true); }) == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(count_events(rig.gpio, Kind::Open) == 1);
}

// Falsified by: power_port_switch_driver.h set_connected() calling set_value() on every port after
// open (a connect that drives the lines).
TEST_CASE("ToupTek StellaVita Switch Driver - connecting leaves every port on and writes nothing",
          "[touptek][switch][unit][fake-gpio]") {
    Rig rig;
    auto driver = rig.make(touptek::default_stellavita_config());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(count_events(rig.gpio, Kind::Open) == 1);
    CHECK(rig.gpio.write_count() == 0);
    for (int i = 0; i < 4; ++i) CHECK(driver->get_switch(i));
    // Only the four configured ports are exposed; the Cypress hub lines are not.
    CHECK(driver->get_max_switch() == 4);
    CHECK(error_code_of([&] { driver->get_switch(4); }) == alpacacore::AlpacaError::InvalidValue);

    driver->set_switch(2, false);
    CHECK_FALSE(driver->get_switch(2));
    REQUIRE(rig.gpio.write_count() == 1);
    CHECK_FALSE(rig.gpio.writes_to(2).at(0).high);
    driver->set_connected(false);
    CHECK(count_events(rig.gpio, Kind::Close) == 1);
    CHECK(error_code_of([&] { driver->get_switch(0); }) == alpacacore::AlpacaError::NotConnected);
}

// Falsified by: power_port_switch_driver.h set_switch_value() dropping the PWM range check (101 reaches the
// engine and the cache).
TEST_CASE("ToupTek StellaVita Switch Driver - PWM and read-only ports map through the engine",
          "[touptek][switch][unit][fake-gpio]") {
    Rig rig;
    auto driver = rig.make(mixed_config());
    driver->set_connected(true);
    CHECK(driver->get_switch_value(1) == Catch::Approx(100.0));
    driver->set_switch_value(1, 35.0);
    CHECK(driver->get_switch_value(1) == Catch::Approx(35.0));
    CHECK(error_code_of([&] { driver->set_switch_value(1, 101.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_switch_value(1) == Catch::Approx(35.0));
    CHECK(error_code_of([&] { driver->set_switch(3, false); }) == alpacacore::AlpacaError::NotImplemented);
    driver->set_connected(false);
}

// Decision 0027: the StellaVita's PWM duty is the engine's, on the task clock. Falsified by:
// touptek_switch_driver.cpp stellavita_ports() not passing pwm_frequency_hz to the engine (the period stays at
// the engine's 50 Hz, the low edge lands at 10 ms), or not passing pwm_enabled (no edges).
TEST_CASE("ToupTek StellaVita Switch Driver - a PWM port bit-bangs its duty at the configured frequency",
          "[touptek][switch][unit][fake-gpio]") {
    using namespace std::chrono_literals;
    Rig rig;
    auto driver = rig.make(mixed_config());  // 100 Hz: a 10 ms period
    driver->set_connected(true);
    const auto base = rig.clock.wait_count();
    driver->set_switch_value(1, 50.0);  // Port 2, backend line index 1
    REQUIRE(rig.clock.wait_for_wait_count(base + 1, 2s));

    auto writes = rig.gpio.writes_to(1);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0].high);

    rig.clock.advance(5ms);
    REQUIRE(rig.clock.wait_for_wait_count(base + 2, 2s));
    writes = rig.gpio.writes_to(1);
    REQUIRE(writes.size() == 2);
    CHECK_FALSE(writes[1].high);
    CHECK(std::chrono::duration_cast<std::chrono::microseconds>(writes[1].at - writes[0].at).count() == 5000);
    driver->set_connected(false);
}

// Issue #300: a connect reads each line instead of assuming the boot left it on.
TEST_CASE("ToupTek StellaVita Switch Driver - connecting reports a port the boot left off as off",
          "[touptek][switch][unit][fake-gpio][power-on]") {
    Rig rig;
    rig.gpio.set_level(0, 0);
    rig.gpio.set_level(1, 0);
    auto driver = rig.make(mixed_config());
    driver->set_connected(true);
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch_value(1) == Catch::Approx(0.0));  // PWM port: 0 %, not 100 %
    CHECK(driver->get_switch(2));
    CHECK(rig.gpio.write_count() == 0);
    driver->set_connected(false);
}

TEST_CASE("ToupTek StellaVita Switch Driver - a client's write is saved per UniqueID and port",
          "[touptek][switch][unit][fake-gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    Rig rig;
    auto driver = rig.make(mixed_config());
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch_value(1, 35.0);
    driver->set_switch(2, true);
    const auto saved = alpacacore::util::PowerPortStateStore::load(driver->get_unique_id());
    CHECK(saved == std::map<int, double>{{0, 0.0}, {1, 35.0}, {2, 100.0}});
    driver->set_connected(false);
}

TEST_CASE(
    "ToupTek StellaVita Switch Driver - restore at server start sets the saved ports and holds them without "
    "connecting",
    "[touptek][switch][unit][fake-gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    auto cfg = mixed_config();
    cfg.power_on_state = alpacacore::util::PowerOnState::Restore;
    Rig rig;
    auto driver = rig.make(cfg);
    REQUIRE(alpacacore::util::PowerPortStateStore::save(driver->get_unique_id(), 0, 0.0));
    REQUIRE(alpacacore::util::PowerPortStateStore::save(driver->get_unique_id(), 1, 35.0));

    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    CHECK(rig.gpio.is_open());  // held: a released PWM line would lose its duty
    REQUIRE(rig.gpio.writes_to(0).size() == 1);
    CHECK_FALSE(rig.gpio.writes_to(0)[0].high);
    CHECK(rig.gpio.writes_to(2).empty());  // nothing saved: left as it was
    CHECK(error_code_of([&] { driver->get_switch(0); }) == alpacacore::AlpacaError::NotConnected);

    // A Disconnect before any Connect keeps the hold.
    driver->set_connected(false);
    CHECK(rig.gpio.is_open());
    CHECK(count_events(rig.gpio, Kind::Close) == 0);

    // The first Connect takes the held lines over as they are.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(count_events(rig.gpio, Kind::Open) == 1);
    CHECK(driver->get_switch_value(1) == Catch::Approx(35.0));
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch(2));
    driver->set_connected(false);
}

TEST_CASE("ToupTek StellaVita Switch Driver - off turns writable ports off after a boot and restores after a restart",
          "[touptek][switch][unit][fake-gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    auto cfg = mixed_config();
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    {
        Rig rig;
        auto driver = rig.make(cfg);
        driver->apply_power_on_state(true);
        // What the boot applied is saved, so a restart cannot bring back an older "on".
        CHECK(alpacacore::util::PowerPortStateStore::load(driver->get_unique_id()) ==
              std::map<int, double>{{0, 0.0}, {1, 0.0}, {2, 0.0}});
        driver->set_connected(true);
        CHECK_FALSE(driver->get_switch(0));
        CHECK_FALSE(driver->get_switch(1));
        CHECK_FALSE(driver->get_switch(2));
        CHECK(driver->get_switch(3));  // read-only: never written
        driver->set_switch(2, true);
        driver->set_connected(false);
    }
    // A service restart in the same boot: the client's last setting comes back.
    Rig rig;
    auto driver = rig.make(cfg);
    driver->apply_power_on_state(false);
    driver->set_connected(true);
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch(2));
    driver->set_connected(false);
}

TEST_CASE("ToupTek StellaVita Switch Driver - leave never touches the backend at server start",
          "[touptek][switch][unit][fake-gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    Rig rig;
    auto driver = rig.make(mixed_config());  // power_on_state defaults to Leave
    REQUIRE(alpacacore::util::PowerPortStateStore::save(driver->get_unique_id(), 0, 0.0));
    driver->apply_power_on_state(true);
    CHECK(count_events(rig.gpio, Kind::Open) == 0);
    CHECK(rig.gpio.write_count() == 0);
}

TEST_CASE(
    "ToupTek StellaVita Switch Driver - the server-start state never changes a port a client already "
    "connected to",
    "[touptek][switch][unit][fake-gpio][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    auto cfg = mixed_config();
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    Rig rig;
    auto driver = rig.make(cfg);
    driver->set_connected(true);
    driver->apply_power_on_state(true);
    CHECK(rig.gpio.write_count() == 0);
    CHECK(driver->get_switch(0));
    driver->set_connected(false);
}
