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

#pragma once

#include <alpacacore/telescope_driver.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace alpacacore::test {

// Drain before asserting: a future's destructor cannot join a virtual sleeper unaided.
inline void drain_cgx_clock_task(std::future<void>& task, FakeTaskClock& clock) {
    using namespace std::chrono_literals;
    const auto watchdog = std::chrono::steady_clock::now() + 10s;
    while (task.wait_for(1ms) != std::future_status::ready) {
        // A watchdog failure must stop the harness, never abandon a fixture-capturing task.
        if (std::chrono::steady_clock::now() >= watchdog) {
            FAIL_CHECK("CGX clock task exceeded the real harness watchdog");
            std::abort();
        }
        if (clock.wait_for_waiters(1, 1ms)) {
            clock.advance(20ms);
            if (!clock.wait_for_woken_settled(2s)) {
                FAIL_CHECK("CGX clock task did not register its next wait or exit");
                std::abort();
            }
        }
    }
}

inline void set_cgx_connected(TelescopeDriver& driver, FakeTaskClock& clock, bool connected) {
    auto task = std::async(std::launch::async, [&] { driver.set_connected(connected); });
    drain_cgx_clock_task(task, clock);
    task.get();
}

// Declared after the driver, so normal and assertion-unwind cleanup precede member destruction.
struct CgxClockCleanup {
    std::unique_ptr<TelescopeDriver>& driver;
    FakeTaskClock& clock;

    ~CgxClockCleanup() {
        if (driver) CHECK_NOTHROW(set_cgx_connected(*driver, clock, false));
    }
};

}  // namespace alpacacore::test
