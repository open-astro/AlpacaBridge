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
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/version.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "power_port_test_dir.h"
#include "zwo_asiair_stubs.h"

// The backend's PWM_GPIO_SET_MODE, ENABLE and SET_LEVEL writes land in the ioctl stub of zwo_asiair_stubs.h when
// the device path is a StubDeviceFile; the default /dev/pwm-gpio-misc still reaches the real ioctl().
namespace {

using alpacacore::test::ioctl_stub;
using alpacacore::test::IoctlStub;
using alpacacore::test::StubDeviceFile;

using alpacacore::test::alpaca_error_message;
using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Defaults", "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO ASIAIR Plus Switch");
    REQUIRE(driver->get_max_switch() == 4);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Device metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        3, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() ==
          "ZWO ASIAIR Plus (RK3568) 12V power switch (/dev/pwm-gpio-misc)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Plus Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_ASIAIR_PLUS_RK3568_3");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Not connected throws",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE_FALSE(driver->get_connected());

    require_alpaca_error([&]() { driver->get_switch(0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(2); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); },
                         alpacacore::AlpacaError::NotConnected);
}

// Issue #2264: the value checks precede the connection check (ASCOM precedence), as on the iMate.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - value range is InvalidValue while disconnected",
          "[zwo][switch][asiair-plus][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    REQUIRE_FALSE(driver->get_connected());

    // An on/off port takes 0-1.
    require_alpaca_error([&]() { driver->set_switch_value(2, 2.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(2, -1.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(2, std::nan("")); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(2, std::numeric_limits<double>::infinity()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); }, alpacacore::AlpacaError::NotConnected);
    // A PWM port takes 0-100.
    require_alpaca_error([&]() { driver->set_switch_value(3, 101.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(3, 100.0); }, alpacacore::AlpacaError::NotConnected);
}

// Falsified by: zwo_asiair_plus_switch_driver.cpp asiair_plus_profile() editing not_connected_message or a port's
// range_error.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - error texts", "[zwo][switch][asiair-plus][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    REQUIRE_FALSE(driver->get_connected());

    CHECK(alpaca_error_message([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected) ==
          "ASIAIR Plus switch not connected");
    CHECK(alpaca_error_message([&]() { driver->set_switch_value(2, 2.0); }, alpacacore::AlpacaError::InvalidValue) ==
          "Switch value out of range");
    CHECK(alpaca_error_message([&]() { driver->set_switch_value(3, 101.0); }, alpacacore::AlpacaError::InvalidValue) ==
          "Switch value out of range");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported actions",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Per-port metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true; // mark Port 4 (DC port 4) as a PWM channel
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);

    REQUIRE(driver->get_max_switch() == 4);

    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
    }

    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Boolean channels have range [0, 1]; the PWM channel exposes [0, 100].
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(3) == Catch::Approx(100.0));

    CHECK(driver->get_switch_description(0) == "ASIAIR Plus DC port 1 (on/off)");
    CHECK(driver->get_switch_description(1) == "ASIAIR Plus DC port 2 (on/off)");
    CHECK(driver->get_switch_description(2) == "ASIAIR Plus DC port 3 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR Plus DC port 4 (PWM 0-100%)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Switch ID range validation",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    require_alpaca_error([&]() { driver->get_switch_name(-1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); },
                         alpacacore::AlpacaError::InvalidValue);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(4); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - State machine when disconnected",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

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

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported method error codes",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    // Async mutation isn't implemented; the disconnected-state check fires first
    // because ASCOM contract requires NotConnected before any other validation.
    require_alpaca_error([&]() { driver->set_async(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); },
                         alpacacore::AlpacaError::NotConnected);

    // Action/Command surfaces are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Constructor rejects invalid configs",
          "[zwo][switch][asiair-plus][unit]") {
    // Empty ports list rejected at construction.
    alpacacore::vendor::zwo::AsiairPlusSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, empty_cfg); },
        alpacacore::AlpacaError::InvalidValue);

    // More than four ports rejected (the hardware only has four DC outputs).
    auto too_many = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    too_many.ports.push_back({"Port 5", false});
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, too_many); },
        alpacacore::AlpacaError::InvalidValue);

    // PWM frequency of 0 Hz is rejected (would divide by zero in the period calc).
    auto zero_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    zero_freq.pwm_frequency_hz = 0;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, zero_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // Absurdly high PWM frequency is rejected (above the empirically validated range).
    auto huge_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    huge_freq.pwm_frequency_hz = 1000001;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, huge_freq); },
        alpacacore::AlpacaError::InvalidValue);
}

// A PWM worker stops wherever it is in the cycle; close() must leave the port at
// the steady level of its duty. Raw SET_LEVEL polarity is inverted: 0 = ON.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - close settles a partial-duty PWM port ON",
          "[zwo][switch][asiair-plus][unit]") {
    char tmpl[] = "/tmp/asiair-plus-stub-XXXXXX";
    const int tfd = ::mkstemp(tmpl);
    REQUIRE(tfd >= 0);
    ::close(tfd);
    auto& st = ioctl_stub();
    // Clear the stub and remove the temp file even when a REQUIRE below throws.
    struct Cleanup {
        const char* path;
        IoctlStub& stub;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(stub.m);
                stub.path.clear();
            }
            ::unlink(path);
        }
    } cleanup{tmpl, st};
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.path = tmpl;
        st.levels.clear();
        st.off_writes = 0;
    }

    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = tmpl;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);

    // Close right after an OFF write: the worker is in the off half of its
    // cycle and exits at the top of the loop without another write.
    {
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return st.off_writes >= 1; }));
    }
    driver->set_connected(false);

    std::vector<std::int32_t> levels;
    {
        std::lock_guard<std::mutex> lock(st.m);
        levels = st.levels;
    }
    REQUIRE_FALSE(levels.empty());
    CHECK(levels.back() == 0);  // duty 50 settles logically ON (raw 0)
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - off at server start writes OFF to every port without connecting",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    char tmpl[] = "/tmp/asiair-plus-stub-XXXXXX";
    const int tfd = ::mkstemp(tmpl);
    REQUIRE(tfd >= 0);
    ::close(tfd);
    auto& st = ioctl_stub();
    struct Cleanup {
        const char* path;
        IoctlStub& stub;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(stub.m);
                stub.path.clear();
            }
            ::unlink(path);
        }
    } cleanup{tmpl, st};
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.path = tmpl;
        st.levels.clear();
        st.off_writes = 0;
    }

    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = tmpl;
    auto leave = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    leave->apply_power_on_state(true);
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.levels.empty());  // the default, Leave, writes nothing
    }

    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(1, cfg);
    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.off_writes == 4);  // raw level 1 = logical OFF, once per port
    }
    driver->set_connected(true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_switch(i));
    }
    driver->set_connected(false);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a client's write is saved per UniqueID and port",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(2, true);
    CHECK(alpacacore::util::PowerPortStateStore::load(driver->get_unique_id()) ==
          std::map<int, double>{{0, 0.0}, {2, 100.0}});
    driver->set_connected(false);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a Disconnect before any Connect keeps the fd a start opened",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->apply_power_on_state(true);
    REQUIRE(stub.open_fds() == 1);
    // A client's Disconnect with nothing connected must not close it (decision 0021 rule 5).
    driver->set_connected(false);
    CHECK(stub.open_fds() == 1);
    driver->set_connected(true);
    driver->set_connected(false);
    CHECK(stub.open_fds() == 0);
}

// Decision 0021 rule 1: a connect must not drive a PWM port to the cached 100 before a client sets it.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - connect writes nothing to a PWM port no client set",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    // Bounded wait for a write that must not come (the worker's first pass is immediate).
    auto& st = ioctl_stub();
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    driver->set_connected(false);
    CHECK(requests == 0);
    // Disconnect must not drive the unset line either: close() settles only a port a client armed.
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.requests == 0);
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a client's PWM write still reaches the fd after connect",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    auto& st = ioctl_stub();
    bool wrote = false;
    {
        std::unique_lock<std::mutex> lock(st.m);
        wrote = st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !st.levels.empty(); });
    }
    driver->set_connected(false);
    CHECK(wrote);
}

// Falsified by: pwm_gpio_misc_backend.cpp write() sending SET_MODE and ENABLE before every SET_LEVEL (dropping the
// initialized_ guard).
TEST_CASE("ZWO ASIAIR Plus Switch Driver - SET_MODE and ENABLE go once per line, SET_LEVEL on every write",
          "[zwo][switch][asiair-plus][unit]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(0, true);
    driver->set_connected(false);
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    CHECK(st.set_modes == std::vector<std::int32_t>{4});
    CHECK(st.enables == std::vector<std::int32_t>{4});
    CHECK(st.levels.size() == 2);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - connect with no PWM port writes nothing",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    auto& st = ioctl_stub();
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    driver->set_connected(false);
    CHECK(requests == 0);
}

// Decision 0021 rule 1: close() settles a PWM line to its steady level, so the next connect reports that level.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - a reconnect reports the level close settled and writes nothing",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    auto& st = ioctl_stub();
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    {
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !st.levels.empty(); }));
    }
    driver->set_connected(false);
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.requests = 0;
    }
    driver->set_connected(true);
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    const double reported = driver->get_switch_value(3);
    driver->set_connected(false);
    CHECK(requests == 0);
    CHECK(reported == 100.0);
}

namespace {

// The raw SET_LEVEL the stub last saw for a kernel index, or -1 when none reached it (0 = ON, 1 = OFF).
int last_raw_level(std::int32_t kernel_index) {
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    const auto it = st.last_level.find(kernel_index);
    return it == st.last_level.end() ? -1 : it->second;
}

}  // namespace

// Issue #1862: every ISwitchV3 member while connected, each value checked against the ioctl stub. The DC ports are
// kernel indices 4..7; Port 4 is a PWM port.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - every ISwitchV3 member answers while connected",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(stub.open_fds() == 1);

    REQUIRE(driver->get_max_switch() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_min_switch_value(i) == 0.0);
        CHECK(driver->get_max_switch_value(i) == (i == 3 ? 100.0 : 1.0));
        CHECK(driver->get_switch_step(i) == 1.0);
        CHECK(driver->get_state_change_complete(i));
        CHECK(driver->get_switch_name(i) == "Port " + std::to_string(i + 1));
        // The module cannot read its pads, so the connect keeps the "on" cache (ascom-contract GPIO rules).
        CHECK(driver->get_switch(i));
    }
    CHECK(driver->get_switch_value(0) == 1.0);
    CHECK(driver->get_switch_value(3) == 100.0);
    CHECK(driver->get_switch_description(3) == "ASIAIR Plus DC port 4 (PWM 0-100%)");

    // Writes reach the module (raw polarity inverted) and read back.
    driver->set_switch(0, false);
    CHECK(last_raw_level(4) == 1);
    CHECK_FALSE(driver->get_switch(0));
    CHECK(driver->get_switch_value(0) == 0.0);
    driver->set_switch_value(1, 1.0);
    CHECK(last_raw_level(5) == 0);
    CHECK(driver->get_switch(1));
    driver->set_switch_value(3, 40.0);
    CHECK(driver->get_switch_value(3) == 40.0);
    CHECK(driver->get_switch(3));

    // Out of range while connected: InvalidValue, and the value stays.
    require_alpaca_error([&] { driver->set_switch_value(0, 2.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(3, 101.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_value(3, std::numeric_limits<double>::quiet_NaN()); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch(4, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->set_switch_name(4, "X"); }, alpacacore::AlpacaError::InvalidValue);
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

    driver->set_switch_name(2, "Flat Panel");
    CHECK(driver->get_switch_name(2) == "Flat Panel");

    // Disconnect leaves every port at its steady level: the boolean ports as commanded, the 40 % port settled ON,
    // and Port 3, which no client wrote, untouched. The fd is closed.
    driver->set_connected(false);
    CHECK(stub.open_fds() == 0);
    CHECK(last_raw_level(4) == 1);
    CHECK(last_raw_level(5) == 0);
    CHECK(last_raw_level(6) == -1);
    CHECK(last_raw_level(7) == 0);
    CHECK(driver->get_switch_name(2) == "Flat Panel");

    // A client that turns every port off before it disconnects finds every port off.
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(driver->get_switch_value(3) == 100.0);  // the level close settled, not the duty
    for (int i = 0; i < 4; ++i) {
        driver->set_switch(i, false);
    }
    driver->set_connected(false);
    for (std::int32_t index = 4; index <= 7; ++index) {
        CHECK(last_raw_level(index) == 1);
    }
}

// ---- The two ioctl numberings (pwm_gpio.h): pwm_gpio_compat gets the corrected one, anything else the legacy one ----
//
// The OpenAstro mainline image replaces ZWO's pwm_gpio.ko with the clean-room pwm_gpio_compat, which implements the
// module's real command numbers. The backend picks the numbering at open() from the device's sysfs driver link; these
// cases point it at a fake /sys/dev/char tree through the factory's sysfs overload, and the stub plays the module the
// tree names (zwo_asiair_stubs.h). The legacy cases above run on the default factory and are unchanged.
namespace {

using alpacacore::test::compat_device;
using alpacacore::test::IoctlCall;
using alpacacore::test::kAbiPwmEnable;
using alpacacore::test::kAbiSetLevel;
using alpacacore::test::kAbiSetMode;
using alpacacore::test::kLegacyEnable;
using alpacacore::test::kLegacySetLevel;
using alpacacore::test::kLegacySetMode;
using alpacacore::test::StubDeviceOptions;
using alpacacore::test::StubModule;

std::unique_ptr<alpacacore::SwitchDriver> make_on(const StubDeviceFile& device,
                                                  alpacacore::vendor::zwo::AsiairPlusSwitchConfig cfg =
                                                      alpacacore::vendor::zwo::default_asiair_plus_rk3568_config()) {
    cfg.device_path = device.path;
    return alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg, device.sysfs_root());
}

std::vector<IoctlCall> stub_calls() {
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    return st.calls;
}

// The legacy stream for a first ON write to DC port 1 (kernel index 4): SET_MODE(GPIO), ENABLE, SET_LEVEL(raw 0).
const std::vector<IoctlCall> kLegacyFirstOnPort1 = {
    {kLegacySetMode, 4, 1, false},
    {kLegacyEnable, 4, 0, false},
    {kLegacySetLevel, 4, 0, false},
};

StubDeviceOptions legacy_device(bool char_device, std::string sysfs_driver) {
    StubDeviceOptions options;
    options.module = StubModule::Legacy;
    options.char_device = char_device;
    options.sysfs_driver = std::move(sysfs_driver);
    return options;
}

// Connects a default driver on a fresh @p options device, sets Port @p port + 1 to @p on, reads it back and
// disconnects; returns every ioctl the stub saw.
std::vector<IoctlCall> calls_of_one_write(const StubDeviceOptions& options, int port, bool on) {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(options);
    auto driver = make_on(device);
    driver->set_connected(true);
    driver->set_switch(port, on);
    CHECK(driver->get_switch(port) == on);
    driver->set_connected(false);
    return stub_calls();
}

}  // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: ON sends SET_MODE(GPIO), then SET_LEVEL(1)",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(compat_device(), 0, true) ==
          std::vector<IoctlCall>{{kAbiSetMode, 4, 1, false}, {kAbiSetLevel, 4, 1, false}});
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: OFF sends SET_LEVEL(0)",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(compat_device(), 1, false) ==
          std::vector<IoctlCall>{{kAbiSetMode, 5, 1, false}, {kAbiSetLevel, 5, 0, false}});
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: no PWM ENABLE or other command reaches the module",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    for (int i = 0; i < 3; ++i) {
        driver->set_switch(i, false);
        driver->set_switch(i, true);
    }
    driver->set_switch_value(3, 0.0);
    driver->set_switch_value(3, 100.0);
    driver->set_connected(false);
    const auto calls = stub_calls();
    REQUIRE_FALSE(calls.empty());
    for (const auto& call : calls) {
        CHECK(call.request != kAbiPwmEnable);
        CHECK((call.request == kAbiSetMode || call.request == kAbiSetLevel));
        CHECK_FALSE(call.refused);
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: SET_MODE once per line since open, SET_LEVEL every write",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto driver = make_on(device);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(0, true);
    driver->set_switch(0, false);
    driver->set_switch(2, true);
    driver->set_connected(false);
    CHECK(stub_calls() == std::vector<IoctlCall>{
                              {kAbiSetMode, 4, 1, false},
                              {kAbiSetLevel, 4, 0, false},
                              {kAbiSetLevel, 4, 1, false},
                              {kAbiSetLevel, 4, 0, false},
                              {kAbiSetMode, 6, 1, false},
                              {kAbiSetLevel, 6, 1, false},
                          });
    // A new open() starts over: the module is told the mode again before the line's next level.
    {
        auto& st = ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        st.calls.clear();
    }
    driver->set_connected(true);
    driver->set_switch(0, true);
    driver->set_connected(false);
    CHECK(stub_calls() == std::vector<IoctlCall>{{kAbiSetMode, 4, 1, false}, {kAbiSetLevel, 4, 1, false}});
}

// Decision 0021 rule 1 and the RK3568 open() rule (zwo.instructions.md): a connect touches no kernel-side state.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: connect and disconnect write nothing",
          "[zwo][switch][asiair-plus][unit][compat][power-on]") {
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(device.open_fds() == 1);
    auto& st = ioctl_stub();
    int requests = 0;
    {
        // Bounded wait for a write that must not come (a PWM worker's first pass is immediate).
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_switch(i));  // the "on" cache: nothing was read either
    }
    driver->set_connected(false);
    CHECK(device.open_fds() == 0);
    CHECK(requests == 0);
    std::lock_guard<std::mutex> lock(st.m);
    CHECK(st.requests == 0);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: soft-PWM edges are SET_LEVEL only and close settles ON",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    auto& st = ioctl_stub();
    {
        // Several edges of the 50 Hz waveform, both levels among them.
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(
            st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return st.off_writes >= 2 && st.levels.size() >= 4; }));
    }
    driver->set_connected(false);
    const auto calls = stub_calls();
    REQUIRE(calls.size() >= 5);
    CHECK(calls.front() == IoctlCall{kAbiSetMode, 7, 1, false});
    for (std::size_t i = 1; i < calls.size(); ++i) {
        CHECK(calls[i].request == kAbiSetLevel);
        CHECK(calls[i].index == 7);
        CHECK((calls[i].value == 0 || calls[i].value == 1));
        CHECK_FALSE(calls[i].refused);
    }
    CHECK(calls.back().value == 1);  // duty 50 settles ON, level 1 on this module
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: off at server start sends SET_LEVEL(0) to every port",
          "[zwo][switch][asiair-plus][unit][compat][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = make_on(device, cfg);
    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    for (std::int32_t index = 4; index <= 7; ++index) {
        CHECK(last_raw_level(index) == 0);
    }
    {
        auto& st = ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.off_writes == 4);
        CHECK(st.refused.empty());
    }
    driver->set_connected(true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_switch(i));
    }
    driver->set_connected(false);
}

// Fail toward stock: only a character device whose sysfs driver is exactly pwm_gpio_compat gets the corrected
// numbering.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - a plain file keeps the legacy numbering whatever sysfs names",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(false, "pwm_gpio_compat"), 0, true) == kLegacyFirstOnPort1);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a character device driven by ZWO's pwm_gpio keeps the legacy numbering",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, "pwm_gpio"), 0, true) == kLegacyFirstOnPort1);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a character device with no sysfs driver link keeps the legacy numbering",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, ""), 0, true) == kLegacyFirstOnPort1);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a driver name that only begins with pwm_gpio_compat keeps the legacy one",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, "pwm_gpio_compat_old"), 0, true) == kLegacyFirstOnPort1);
}
