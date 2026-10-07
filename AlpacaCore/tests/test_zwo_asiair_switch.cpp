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
#include <alpacacore/vendor/zwo/zwo_asiair_protocol_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/version.h>
#include <dlfcn.h>
#include <gpiod.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#include "catch2_compat.h"

// ---------------------------------------------------------------------------
// libgpiod stub (issue #772). This binary defines the five libgpiod calls that
// reach a GPIO chip, so the wrappers linked into it call these instead of the
// shared library's. For the chip path kStubChip they model a chip in memory
// and record the level each line held when its request was released; for any
// other chip, request or path they forward to the real libgpiod (found with
// RTLD_NEXT), so every other test in the binary sees the real library. The
// settings and config objects are plain user-space structs and stay real.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kStubChip = "/dev/gpiochip-asiair-stub";
constexpr unsigned int kStubLines = 64;

struct GpiodStub {
    std::mutex m;
    std::array<int, kStubLines> level{};
    std::array<int, kStubLines> level_at_release{};
    char chip = 0;     // identity of the stub chip
    char request = 0;  // identity of the stub request
};

GpiodStub& stub() {
    static GpiodStub s;
    return s;
}

gpiod_chip* stub_chip() { return reinterpret_cast<gpiod_chip*>(&stub().chip); }
gpiod_line_request* stub_request() { return reinterpret_cast<gpiod_line_request*>(&stub().request); }

template <typename Fn>
Fn real_gpiod(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

int stub_level_at_release(unsigned int offset) {
    std::lock_guard<std::mutex> lock(stub().m);
    return offset < kStubLines ? stub().level_at_release[offset] : -1;
}

}  // namespace

extern "C" {

gpiod_chip* gpiod_chip_open(const char* path) {
    if (path != nullptr && std::strcmp(path, kStubChip) == 0) {
        return stub_chip();
    }
    static const auto real = real_gpiod<gpiod_chip* (*)(const char*)>("gpiod_chip_open");
    if (real == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }
    return real(path);
}

void gpiod_chip_close(gpiod_chip* chip) {
    if (chip == stub_chip()) {
        return;
    }
    static const auto real = real_gpiod<void (*)(gpiod_chip*)>("gpiod_chip_close");
    if (real != nullptr) {
        real(chip);
    }
}

gpiod_line_request* gpiod_chip_request_lines(gpiod_chip* chip, gpiod_request_config* req_cfg,
                                             gpiod_line_config* line_cfg) {
    if (chip == stub_chip()) {
        std::lock_guard<std::mutex> lock(stub().m);
        stub().level.fill(1);  // every ASIAIR line is requested high
        stub().level_at_release.fill(-1);
        return stub_request();
    }
    static const auto real =
        real_gpiod<gpiod_line_request* (*)(gpiod_chip*, gpiod_request_config*, gpiod_line_config*)>(
            "gpiod_chip_request_lines");
    if (real == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }
    return real(chip, req_cfg, line_cfg);
}

int gpiod_line_request_set_value(gpiod_line_request* request, unsigned int offset, gpiod_line_value value) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        if (offset < kStubLines) {
            stub().level[offset] = value == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
        }
        return 0;
    }
    static const auto real =
        real_gpiod<int (*)(gpiod_line_request*, unsigned int, gpiod_line_value)>("gpiod_line_request_set_value");
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(request, offset, value);
}

void gpiod_line_request_release(gpiod_line_request* request) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        stub().level_at_release = stub().level;
        return;
    }
    static const auto real = real_gpiod<void (*)(gpiod_line_request*)>("gpiod_line_request_release");
    if (real != nullptr) {
        real(request);
    }
}

}  // extern "C"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

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
    config.model_name = "ASIAIR Plus (Pi CM4)";
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_switch(0, config);

    REQUIRE(driver != nullptr);
    CHECK(driver->get_name() == "ZWO ASIAIR Plus (Pi CM4) Switch");
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

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
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
// half the time and the heater on it loses power at disconnect. AGENTS.md
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
