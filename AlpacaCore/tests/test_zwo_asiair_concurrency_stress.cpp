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

// Connect/disconnect/operate concurrency stress for the two ZWO ASIAIR switch drivers (issue #1862): the ASIAIR
// Pro (and Plus Pi CM4) over the libgpiod stub chip and the ASIAIR Plus RK3568 over the ioctl stub, both in
// zwo_asiair_stubs.h. Each connect really succeeds before the storm, so the storm reaches the boolean writes, the
// PWM worker threads and the racing disconnect, not only a failed open. The stub chip refuses a second request
// while one is live and a write through a released request, so a double claim or a write after release fails a
// case. Operations racing a disconnect are EXPECTED to throw NotConnected; what must never happen is a crash, a
// hang, an unreleased claim or a TSan report.

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
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
    const int requests_before = alpacacore::test::stub_requests();

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
    CHECK(alpacacore::test::stub_releases() == alpacacore::test::stub_requests());
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (alpacacore::test::stub_level(18) != 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();  // real time: the worker's 1 kHz cycle
        }
        REQUIRE(alpacacore::test::stub_level(18) == 0);
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
