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
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "power_port_test_dir.h"
#include "zwo_asiair_stubs.h"

// The wrapper's PWM_GPIO_SET_MODE, ENABLE and SET_LEVEL writes land in the ioctl stub of zwo_asiair_stubs.h when
// the device path is a StubDeviceFile; the default /dev/pwm-gpio-misc still reaches the real ioctl().
namespace {

using alpacacore::test::ioctl_stub;
using alpacacore::test::IoctlStub;
using alpacacore::test::StubDeviceFile;

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
