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

// Helpers and a polled-driver rig every driver test file shares: free functions, plus PolledRig;
// a file includes this header instead of keeping its own copy.

#include <alpacacore/alpacadriver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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

// The poll period and the real-time bound a PolledRig ticks and parks with.
struct RigTiming {
    std::chrono::nanoseconds period;
    std::chrono::milliseconds bound;
};

// A fake clock, a fake and the driver under test, built in that order and destroyed in reverse (the driver goes
// first). A thin wrapper over connect_and_park(), tick() and require_alpaca_error(): it adds no rendezvous logic.
// The factory builds the driver without connecting it, so a case can set up the fake before connect().
template <typename Fake, typename Driver>
class PolledRig {
public:
    using Factory = std::function<std::unique_ptr<Driver>(Fake&, FakeTaskClock&)>;

    PolledRig(RigTiming timing, Factory make) : timing_(timing), driver_(make(fake_, clock_)) {}

    PolledRig(const PolledRig&) = delete;
    PolledRig& operator=(const PolledRig&) = delete;

    ~PolledRig() {
        try {
            if (driver_ && driver_->get_connected()) driver_->set_connected(false);
        } catch (...) {  // a destructor must not throw
        }
    }

    // connect_and_park(): whether the poll body parked on the clock. REQUIRE(rig.connect()) to fail on a miss.
    bool connect() { return connect_and_park(*driver_, clock_, timing_.bound); }

    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) alpacacore::test::tick(clock_, timing_.period, timing_.bound);
    }

    void expect_error(int code, const std::function<void()>& fn) { require_alpaca_error(fn, code); }

    Fake& fake() { return fake_; }
    FakeTaskClock& clock() { return clock_; }
    Driver& driver() { return *driver_; }

private:
    RigTiming timing_;
    FakeTaskClock clock_;
    Fake fake_;
    std::unique_ptr<Driver> driver_;
};

// A by-id style symlink a serial driver is configured with, so a replugged device (a new pty fake on a new pty)
// comes back under the same name, as a by-id path does. The driver never holds the raw /dev/pts/N, which devpts
// reissues once freed. Works with any fake that has slave_path().
struct ReplugPort {
    std::filesystem::path path;
    template <typename Fake>
    explicit ReplugPort(const Fake& fake) {
        static std::atomic<int> serial{0};
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-replug-" + std::to_string(::getpid()) + "-" + std::to_string(serial.fetch_add(1)));
        std::filesystem::create_symlink(fake.slave_path(), path);
    }
    template <typename Fake>
    void replug(const Fake& fake) {
        std::filesystem::remove(path);
        std::filesystem::create_symlink(fake.slave_path(), path);
    }
    ~ReplugPort() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ReplugPort(const ReplugPort&) = delete;
    ReplugPort& operator=(const ReplugPort&) = delete;
};

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
