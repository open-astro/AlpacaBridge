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

#include <alpacacore/alpacadriver.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "driver_rig.h"

// Helpers for the getters-never-wait tests (decision 0016): a fake holds one device call open,
// the test waits until that call is in flight, then times a getter that needs no device access
// (`timed` from driver_rig.h).
namespace alpacacore::test {

// Budget a no-I/O getter must meet while a device transaction of 200 ms or more is in flight.
inline constexpr std::chrono::milliseconds kGetterBudget{20};

// How long a fake holds a device call open: well past kGetterBudget, well under the settle budgets.
inline constexpr std::chrono::milliseconds kHoldOpen{300};

/// Polls `flag` until it is set or `budget` runs out; false means the fake call never started.
inline bool wait_for_flag(const std::atomic<bool>& flag, std::chrono::milliseconds budget = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// Polls until the async connect task is done and the driver reports Connected.
inline bool wait_until_connected(AlpacaDriver& driver, std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.get_connected() && !driver.get_connecting()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace alpacacore::test
