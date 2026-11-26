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

// The soft-PWM engine under both switch drivers has its own case at the end (issue #1757), on the fake backend.
//
// Connect/disconnect/operate concurrency stress for the two ZWO ASIAIR switch drivers (issue #1862): the ASIAIR
// Pro (and Plus Pi CM4) over the libgpiod stub chip and the ASIAIR Plus RK3568 over the ioctl stub, both in
// zwo_asiair_stubs.h. Each connect really succeeds before the storm, so the storm reaches the boolean writes, the
// PWM worker threads and the racing disconnect, not only a failed open. The stub chip refuses a second request
// while one is live and a write through a released request, so a double claim or a write after release fails a
// case. Operations racing a disconnect are EXPECTED to throw NotConnected; what must never happen is a crash, a
// hang, an unreleased claim or a TSan report.

#include <alpacacore/switch_driver.h>
#include <alpacacore/util/soft_pwm_ports.h>
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_gpio_line_backend.h"
#include "fake_task_clock.h"
#include "power_port_test_dir.h"
#include "zwo_asiair_stubs.h"

using alpacacore::AlpacaDriver;

namespace {

// Every ISwitchV3 member, on a boolean port (0) and the PWM port (3).
void switch_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
    guard([&] { static_cast<void>(sw.get_max_switch()); });
    guard([&] { static_cast<void>(sw.get_can_write(0)); });
    guard([&] { static_cast<void>(sw.get_can_async(3)); });
    guard([&] { static_cast<void>(sw.get_switch_description(3)); });
    guard([&] { static_cast<void>(sw.get_min_switch_value(3)); });
    guard([&] { static_cast<void>(sw.get_max_switch_value(3)); });
    guard([&] { static_cast<void>(sw.get_switch_step(3)); });
    guard([&] { sw.set_async(0, true); });
    guard([&] { sw.set_async_value(3, 50.0); });
    guard([&] { static_cast<void>(sw.get_switch(0)); });
    guard([&] { sw.set_switch(0, true); });
    guard([&] { static_cast<void>(sw.get_switch_value(0)); });
    guard([&] { sw.set_switch_value(0, 0.0); });
    guard([&] { sw.set_switch_value(3, 50.0); });
    guard([&] { static_cast<void>(sw.get_switch_value(3)); });
    guard([&] { static_cast<void>(sw.get_state_change_complete(3)); });
    guard([&] { sw.set_switch_name(1, "Dew"); });
    guard([&] { static_cast<void>(sw.get_switch_name(1)); });
    guard([&] { static_cast<void>(sw.get_device_state()); });
}

std::unique_ptr<alpacacore::SwitchDriver> make_pro() {
    auto cfg = alpacacore::vendor::zwo::default_asiair_pro_config();
    cfg.gpio_chip_path = alpacacore::test::kStubChip;
    cfg.ports[3].pwm_enabled = true;
    return alpacacore::vendor::zwo::create_zwo_asiair_switch(0, cfg);
}

std::unique_ptr<alpacacore::SwitchDriver> make_plus(const alpacacore::test::StubDeviceFile& file) {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = file.path;
    cfg.ports[3].pwm_enabled = true;
    return alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
}

}  // namespace

TEST_CASE("ZWO ASIAIR Pro switch - concurrent connect/disconnect/operate stress", "[zwo][switch][stress]") {
    alpacacore::test::TempPowerPortDir dir;  // every write saves the port's level
    alpacacore::test::stub_reset_levels(1);
    auto driver = make_pro();
    // Counted from here: the stub counts since the binary started, so a claim an earlier case leaked stays out.
    const int requests_before = alpacacore::test::stub_requests();
    const int releases_before = alpacacore::test::stub_releases();

    // The connect really succeeds before the storm.
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(alpacacore::test::stub_claimed());

    // NotConnected from a call racing a disconnect, and the shared NotImplemented code from the async writes
    // (CanAsync is false on every port); every other member answers on a connected driver.
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { switch_operate(guard, d); });

    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    // Every claim the storm made was released: nothing is left holding the lines.
    CHECK_FALSE(alpacacore::test::stub_claimed());
    CHECK(alpacacore::test::stub_releases() - releases_before == alpacacore::test::stub_requests() - requests_before);
    CHECK(alpacacore::test::stub_requests() > requests_before);
}

TEST_CASE("ZWO ASIAIR Pro switch - destruction mid-operation (PWM worker live)", "[zwo][switch][stress]") {
    alpacacore::test::TempPowerPortDir dir;
    for (int i = 0; i < 10; ++i) {
        alpacacore::test::stub_reset_levels(1);
        auto driver = make_pro();
        REQUIRE(alpacacore::test::settle_connected(*driver, true));
        driver->set_switch_value(3, 50.0);  // the PWM worker toggles GPIO 18
        // Destroy it in the low half of its cycle, where a worker that is only stopped leaves the line.
        // Require the low level this loop saw, not a second read: the worker can drive the line high
        // again between two reads (the nightly sanitizer run caught it, #2267).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int seen = alpacacore::test::stub_level(18);
        while (seen != 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();  // real time: the worker's 1 kHz cycle
            seen = alpacacore::test::stub_level(18);
        }
        REQUIRE(seen == 0);
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction.
            driver->disconnect();
        }
        driver.reset();
        // The worker was joined and the lines released, the 50 % port at its steady level (high).
        CHECK_FALSE(alpacacore::test::stub_claimed());
        CHECK(alpacacore::test::stub_level_at_release(18) == 1);
    }
}

TEST_CASE("ZWO ASIAIR Plus switch - concurrent connect/disconnect/operate stress", "[zwo][switch][stress]") {
    alpacacore::test::TempPowerPortDir dir;
    alpacacore::test::StubDeviceFile file;
    auto driver = make_plus(file);

    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(file.open_fds() == 1);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { switch_operate(guard, d); });

    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    // The storm reached the module, and the last disconnect closed the fd it opened.
    {
        auto& st = alpacacore::test::ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.requests > 0);
    }
    CHECK(file.open_fds() == 0);
}

TEST_CASE("ZWO ASIAIR Plus switch - destruction mid-operation (PWM worker live)", "[zwo][switch][stress]") {
    alpacacore::test::TempPowerPortDir dir;
    alpacacore::test::StubDeviceFile file;
    auto& st = alpacacore::test::ioctl_stub();
    for (int i = 0; i < 10; ++i) {
        auto driver = make_plus(file);
        REQUIRE(alpacacore::test::settle_connected(*driver, true));
        int off_writes = 0;
        {
            std::lock_guard<std::mutex> lock(st.m);
            off_writes = st.off_writes;
        }
        driver->set_switch_value(3, 50.0);  // the PWM worker toggles kernel index 7
        {
            // Destroy it in the off half of its cycle: the worker has written OFF and exits there.
            std::unique_lock<std::mutex> lock(st.m);
            REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return st.off_writes > off_writes; }));
        }
        if ((i % 2) != 0) {
            driver->disconnect();
        }
        driver.reset();
        // The worker was joined and the fd closed, the 50 % port settled ON (raw 0).
        CHECK(file.open_fds() == 0);
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.last_level[7] == 0);
    }
}

// Issue #1757: util::SoftPwmPorts, the engine under both drivers, over FakeGpioLineBackend on a virtual clock that a
// ticker thread keeps advancing, so the PWM workers really cycle. Four threads storm set_value() and get_value()
// on boolean and PWM ports while the main thread opens and closes the engine: every call either answers or throws
// NotConnected, and the backend sees no write outside an open/close pair (a write through a released request is
// what the libgpiod stub chip refuses). Under TSan this is the data-race check on the engine's three locks.
// Falsified by: soft_pwm_ports.cpp set_value() dropping the open check before its backend write (a write outside
// an open/close pair: 164-248 failed checks per run). The io_mutex_ window has its own case,
// "SoftPwmPorts - close waits for a boolean write in flight".
TEST_CASE("SoftPwmPorts - set_value storm during close", "[util][soft-pwm][stress]") {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeGpioLineBackend backend(clock);
    alpacacore::util::PortsConfig cfg;
    cfg.device_path = "/dev/gpiochip-fake";
    cfg.ports = {{"A", 12u, true, false}, {"B", 13u, true, false}, {"C", 26u, true, true}, {"D", 18u, true, true}};
    cfg.pwm_frequency_hz = 1000;
    cfg.model_name = "Fake Board";
    cfg.log_category = "SOFT_PWM_TEST";
    alpacacore::util::SoftPwmPorts ports(cfg, backend, clock);

    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    std::atomic<bool> done{false};
    std::thread ticker([&] {
        while (!done.load()) {
            clock.advance(std::chrono::milliseconds(1));
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> setters;
    for (int t = 0; t < 4; ++t) {
        setters.emplace_back([&, t] {
            for (int i = 0; !done.load(); ++i) {
                guard([&] { ports.set_value(static_cast<std::size_t>(t % 2), (i + t) % 2); });
                guard([&] { ports.set_value(2, (i * 7 + t) % 101); });
                guard([&] { ports.set_value(3, i % 2 == 0 ? 0 : 100); });
                guard([&] { static_cast<void>(ports.get_value(static_cast<std::size_t>(i % 4))); });
                std::this_thread::yield();
            }
        });
    }
    for (int cycle = 0; cycle < 100; ++cycle) {
        ports.open();
        std::this_thread::yield();
        ports.close();
    }
    done.store(true);
    for (auto& t : setters) {
        t.join();
    }
    ticker.join();

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    CHECK_FALSE(ports.is_open());
    // No write outside an open/close pair, and every open was closed.
    bool open = false;
    for (const auto& e : backend.events()) {
        using Kind = alpacacore::test::FakeGpioLineBackend::Event::Kind;
        if (e.kind == Kind::Open) {
            CHECK_FALSE(open);
            open = true;
        } else if (e.kind == Kind::Close) {
            CHECK(open);
            open = false;
        } else {
            CHECK(open);
        }
    }
    CHECK_FALSE(open);
}
