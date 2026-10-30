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

// Helpers every driver test file shares. Free functions, no state; a file
// includes this header instead of keeping its own copy.

#include <alpacacore/alpacadriver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace alpacacore::test {

// Fails the test case unless fn throws an AlpacaException with expected_code.
// A bare CHECK_THROWS_AS(..., AlpacaException) passes for any code.
inline void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// True once a Park has ended: the mount is parked, or a read reports the failure.
inline bool park_done(alpacacore::TelescopeDriver& d) {
    try {
        return d.get_at_park() || !d.get_slewing();
    } catch (const alpacacore::AlpacaException&) {
        return true;
    }
}

// Wall-clock time one call takes.
template <typename Fn>
std::chrono::microseconds timed(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
}

// Advances the fake clock one poll period, then waits (up to `bound` of real time) until the woken poll
// body is parked on the clock again. Fails the test case when it does not settle or park.
inline void tick(FakeTaskClock& clock, std::chrono::nanoseconds period, std::chrono::milliseconds bound) {
    clock.advance(period);
    REQUIRE(clock.wait_for_woken_settled(bound));
    REQUIRE(clock.wait_for_waiters(1, bound));
}

// Connects the driver, then waits (up to `bound` of real time) for its poll body to park on the clock.
// Returns whether it parked; a caller that must fail on a missed rendezvous wraps the call in REQUIRE.
inline bool connect_and_park(alpacacore::AlpacaDriver& driver, FakeTaskClock& clock, std::chrono::milliseconds bound) {
    driver.set_connected(true);
    return clock.wait_for_waiters(1, bound);
}

// "2026-10-10T21:46:30.626Z" -> system_clock time point. Fails the test case on any other shape.
inline std::chrono::system_clock::time_point parse_timestamp(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    REQUIRE(std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &y, &mo, &d, &h, &mi, &s, &ms) == 7);
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm)) + std::chrono::milliseconds(ms);
}

// The "TimeStamp" entry of a DeviceState list, empty when there is none.
inline std::string time_stamp_of(const std::vector<alpacacore::DeviceState>& state) {
    for (const auto& entry : state) {
        if (entry.name == "TimeStamp") return std::get<std::string>(entry.value);
    }
    return {};
}

inline std::string time_stamp_of(const alpacacore::AlpacaDriver& driver) {
    return time_stamp_of(driver.get_device_state());
}

}  // namespace alpacacore::test
