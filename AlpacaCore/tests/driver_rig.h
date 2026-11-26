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
#include <alpacacore/util/host_clock_trust.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace alpacacore::test {

// Issue #2419: pins whether the host clock is trusted for a mount's own clock (util::HostClockTrust): the kernel
// probe answers `synchronized` and the latch starts clear; both are restored on exit. A case whose mount sets its clock
// from the host at connect holds one, so it does not depend on the runner's NTP state.
struct HostClockTrustGuard {
    explicit HostClockTrustGuard(bool synchronized) {
        util::HostClockTrust::reset_for_testing();
        util::ClientUtcWarning::set_host_synchronized_probe([synchronized] { return synchronized; });
    }
    ~HostClockTrustGuard() {
        util::ClientUtcWarning::set_host_synchronized_probe(nullptr);
        util::HostClockTrust::reset_for_testing();
    }
    HostClockTrustGuard(const HostClockTrustGuard&) = delete;
    HostClockTrustGuard& operator=(const HostClockTrustGuard&) = delete;
};

// A latch a test seam blocks on. The wait has a real-time bound, so a regression fails the case instead of hanging it.
struct Latch {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;

    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        static_cast<void>(cv.wait_for(lock, std::chrono::seconds(2), [&] { return open; }));
    }
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

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

// The Alpaca error code fn throws, or 0 when it returns (ASCOM "no error"). When message is not null it receives
// the exception's text. A non-Alpaca exception is not caught: on a test thread it fails the case. A call on a
// worker thread, where an escaping exception ends the whole run, is worker_code_of (or sits in a catch-all).
inline int error_code_of(const std::function<void()>& fn, std::string* message = nullptr) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        if (message != nullptr) *message = ex.what();
        return ex.error_code();
    }
    return 0;
}

// error_code_of for a thread other than the test thread, where an escaping exception ends the whole run
// instead of failing one case: a non-Alpaca exception returns -1. Never call it on the test thread.
inline int worker_code_of(const std::function<void()>& fn) {
    try {
        return error_code_of(fn);
    } catch (...) {
        return -1;
    }
}

// The message of the AlpacaException fn throws, after requiring expected_code.
inline std::string alpaca_error_message(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
        return ex.what();
    }
    FAIL("Expected AlpacaException");
    return {};
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

// Runs `fn`, a call that waits on `clock` (a verified AbortSlew, #2412), on its own thread and steps the clock by
// `step` until it returns, then rethrows what it threw. Fails the case when it has not returned after 60 s of real
// time (the thread is released first, so it can be joined).
template <typename Fn>
void call_stepping(FakeTaskClock& clock, Fn&& fn, std::chrono::nanoseconds step = std::chrono::milliseconds(50)) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        done.store(true);
    });
    const bool returned = clock.drain_until([&] { return done.load(); }, step, std::chrono::seconds(60));
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    caller.join();
    REQUIRE(returned);
    if (error) std::rethrow_exception(error);
}

// Advances the fake clock one poll period, then waits (up to `bound` of real time) until the woken poll
// body is parked on the clock again. Fails the test case when it does not settle or park.
inline void tick(FakeTaskClock& clock, std::chrono::nanoseconds period, std::chrono::milliseconds bound) {
    clock.advance(period);
    REQUIRE(clock.wait_for_woken_settled(bound));
    REQUIRE(clock.wait_for_waiters(1, bound));
}

// A driver's poll thread may not have parked on the clock yet when a case advances it right after the connect, and an
// advance that lands before the thread registers its wait is lost to it (its first tick is then a period after the
// later time). So the first advance waits, up to a second of real time, for any waiter to be there. Only a driver whose
// poll has ended (a lost link) pays the second, and nothing is lost by it.
inline void wait_for_a_parked_waiter(FakeTaskClock& clock) {
    static_cast<void>(clock.wait_for_waiters(1, std::chrono::seconds(1)));
}

// Ages the clock past a driver's 30 s post-connect grace (31 s after `connected_at`, the clock's current time by
// default) in steps of at most 5 s, waiting after each for what it woke to park again: a driver with a poll samples at
// each step, so no getter reads a frame older than util::kLinkStalenessBound, which one 31 s jump would leave it.
inline void age_past_connect_grace(FakeTaskClock& clock, FakeTaskClock::clock::time_point connected_at,
                                   std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    constexpr auto kStep = std::chrono::seconds(5);
    const auto target = connected_at + std::chrono::seconds(31);
    wait_for_a_parked_waiter(clock);
    while (clock.now() < target) {
        clock.advance(std::min<std::chrono::nanoseconds>(kStep, target - clock.now()));
        static_cast<void>(clock.wait_for_woken_settled(bound));
    }
}

inline void age_past_connect_grace(FakeTaskClock& clock) { age_past_connect_grace(clock, clock.now()); }

// A connected iOptron telescope parks two tasks on its clock: the state poll and the clock-sync body.
// wait_for_a_parked_waiter() returns on the first, so a step taken before the poll has parked is lost to it, and its
// connect frame then ages past util::kLinkStalenessBound (a getter reads "link lost"). Call it after the connect and
// before the first step.
inline constexpr std::size_t kIoptronConnectWaiters = 2;
inline void wait_for_ioptron_connect_parked(FakeTaskClock& clock,
                                            std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    REQUIRE(clock.wait_for_waiters(kIoptronConnectWaiters, bound));
}

// Advances the clock by `d` in steps of at most 5 s, waiting after each for what it woke to park again: a driver with
// a poll samples at each step, so a getter that reads afterwards serves a frame measured at the end of `d`, and none
// is older than util::kLinkStalenessBound (one long jump would leave it so). Each step of 250 ms or more is one poll
// cycle (the poll sleeps one period per wake and skips the ticks a step spans, with no catch-up).
inline void advance_polling(FakeTaskClock& clock, std::chrono::nanoseconds d,
                            std::chrono::milliseconds bound = std::chrono::seconds(5)) {
    constexpr auto kStep = std::chrono::seconds(5);
    const auto target = clock.now() + d;
    wait_for_a_parked_waiter(clock);
    while (clock.now() < target) {
        clock.advance(std::min<std::chrono::nanoseconds>(kStep, target - clock.now()));
        static_cast<void>(clock.wait_for_woken_settled(bound));
    }
}

// Connects the driver, then waits (up to `bound` of real time) for its poll body, and the `waiters` - 1 other tasks its
// connect starts on the clock, to park. Returns whether they parked; a caller that must fail on a missed rendezvous
// wraps the call in REQUIRE.
inline bool connect_and_park(alpacacore::AlpacaDriver& driver, FakeTaskClock& clock, std::chrono::milliseconds bound,
                             std::size_t waiters = 1) {
    driver.set_connected(true);
    return clock.wait_for_waiters(waiters, bound);
}

// The poll period and the real-time bound a PolledRig ticks and parks with.
struct RigTiming {
    std::chrono::nanoseconds period;
    std::chrono::milliseconds bound;
    std::size_t waiters = 1;  // the tasks a connect parks on the clock (kIoptronConnectWaiters for iOptron)
};

// A fake clock, a fake and the driver under test, built in that order and destroyed in reverse (the driver goes
// first). A thin wrapper over connect_and_park(), tick() and require_alpaca_error(): it adds no rendezvous logic.
// The factory builds the driver without connecting it, so a case can set up the fake before connect().
// pause() and poll_now() forward to the driver's own poll seams, which a rig of a driver that has them passes in; a
// call to one the rig was not given fails the test case.
template <typename Fake, typename Driver>
class PolledRig {
public:
    using Factory = std::function<std::unique_ptr<Driver>(Fake&, FakeTaskClock&)>;
    using PauseSeam = std::function<void(Driver&, bool)>;
    using PollNowSeam = std::function<void(Driver&)>;

    PolledRig(RigTiming timing, Factory make, PauseSeam pause = {}, PollNowSeam poll_now = {})
        : timing_(timing), pause_(std::move(pause)), poll_now_(std::move(poll_now)), driver_(make(fake_, clock_)) {}

    PolledRig(const PolledRig&) = delete;
    PolledRig& operator=(const PolledRig&) = delete;

    ~PolledRig() {
        try {
            if (driver_ && driver_->get_connected()) driver_->set_connected(false);
        } catch (...) {  // a destructor must not throw
        }
    }

    // connect_and_park(): whether the poll body parked on the clock. REQUIRE(rig.connect()) to fail on a miss.
    bool connect() { return connect_and_park(*driver_, clock_, timing_.bound, timing_.waiters); }

    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) alpacacore::test::tick(clock_, timing_.period, timing_.bound);
    }

    // The pause seam: a paused poll skips its cycles but keeps waiting on the clock.
    void pause(bool on) {
        if (!pause_) FAIL("PolledRig: no pause seam");
        pause_(*driver_, on);
    }
    // The poll-now seam: one poll cycle on the calling thread.
    void poll_now() {
        if (!poll_now_) FAIL("PolledRig: no poll_now seam");
        poll_now_(*driver_);
    }

    void expect_error(int code, const std::function<void()>& fn) { require_alpaca_error(fn, code); }

    Fake& fake() { return fake_; }
    FakeTaskClock& clock() { return clock_; }
    Driver& driver() { return *driver_; }

private:
    RigTiming timing_;
    PauseSeam pause_;
    PollNowSeam poll_now_;
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
