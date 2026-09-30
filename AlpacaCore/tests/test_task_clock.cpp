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

// Tests for util::TaskClock, its real adapter and the virtual-time fake
// (issue #697, decision record docs/decisions/0005-task-clock.md).
//
// Waits go through a TaskClock&, never through the concrete class; only
// advance(), wait_for_waiters() and the before_block hook use the fake
// directly. Every blocking step carries a real-time bound (a future waited
// with a deadline, or wait_for_waiters(n, 2s)), so a defect fails the case
// instead of hanging the binary.

#include <alpacacore/util/task_clock.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "catch2_compat.h"
#include "fake_task_clock.h"

using alpacacore::test::FakeTaskClock;
using alpacacore::util::default_task_clock;
using alpacacore::util::RealTaskClock;
using alpacacore::util::TaskClock;
using namespace std::chrono_literals;

// Case 6: nothing owns a clock through the interface, so `delete` through a
// TaskClock* must not compile (protected, non-virtual destructor).
static_assert(!std::is_destructible_v<TaskClock>, "TaskClock must not have a public destructor");

namespace {

constexpr auto kBound = 2s;

// One thread blocked in TaskClock::wait_for on its own mutex, condition
// variable and cancel flag. The result arrives through a future so the test
// can wait for it with a real-time bound. The destructor cancels and joins,
// so a case that fails mid-way still tears down without hanging.
class Waiter {
public:
    Waiter(TaskClock& clock, std::chrono::nanoseconds timeout) {
        auto result = std::make_shared<std::promise<bool>>();
        future_ = result->get_future();
        thread_ = std::thread([this, &clock, timeout, result] {
            std::unique_lock<std::mutex> lock(mutex_);
            result->set_value(clock.wait_for(lock, cv_, timeout, [this] {
                if (!cancel_) {
                    false_evaluations_.fetch_add(1);
                }
                return cancel_;
            }));
        });
    }
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    ~Waiter() {
        cancel(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Sets the flag under the waiter's mutex, as a driver's cancel does.
    void cancel(bool notify) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_ = true;
        }
        if (notify) {
            cv_.notify_all();
        }
    }

    // True once the waiter has evaluated its predicate false. The predicate
    // runs under the waiter's mutex, so after this a cancel() cannot take the
    // mutex until the waiter has released it inside the condition variable
    // wait: the notify then reaches a blocked waiter.
    bool blocked_within(std::chrono::milliseconds bound) {
        const auto give_up = std::chrono::steady_clock::now() + bound;
        while (false_evaluations_.load() == 0) {
            if (std::chrono::steady_clock::now() >= give_up) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    bool finished_within(std::chrono::milliseconds bound) {
        return future_.wait_for(bound) == std::future_status::ready;
    }
    bool result() { return future_.get(); }

    // The mutex the waiter passes to wait_for, for a case that must hold
    // advance() out of its visit to this waiter.
    std::mutex& mutex() { return mutex_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancel_ = false;
    std::atomic<int> false_evaluations_{0};
    std::future<bool> future_;
    std::thread thread_;
};

// One thread in TaskClock::sleep_for. The fake's sleeper can only be released
// by advance(), so a failed case advances the fake far enough in the
// destructor to let the thread go. `before_sleep`, empty by default, runs on
// the thread just before sleep_for.
class Sleeper {
public:
    Sleeper(TaskClock& clock, std::chrono::nanoseconds duration, FakeTaskClock* release_with,
            std::function<void()> before_sleep = {})
        : release_with_(release_with) {
        auto done = std::make_shared<std::promise<void>>();
        future_ = done->get_future();
        thread_ = std::thread([&clock, duration, done, before_sleep = std::move(before_sleep)] {
            if (before_sleep) {
                before_sleep();
            }
            clock.sleep_for(duration);
            done->set_value();
        });
    }
    Sleeper(const Sleeper&) = delete;
    Sleeper& operator=(const Sleeper&) = delete;
    // The thread may not have reached sleep_for yet: an advance before it
    // registers moves the time but leaves its deadline ahead. So advance
    // until the thread finishes, bounded in real time, and abort rather than
    // join a thread that is still blocked.
    ~Sleeper() {
        if (release_with_ != nullptr) {
            const auto give_up = std::chrono::steady_clock::now() + kBound;
            while (!finished_within(1ms)) {
                if (std::chrono::steady_clock::now() >= give_up) {
                    std::fprintf(stderr, "Sleeper: thread not released within the real-time bound\n");
                    std::abort();
                }
                release_with_->advance(24h);
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool finished_within(std::chrono::milliseconds bound) {
        return future_.wait_for(bound) == std::future_status::ready;
    }

private:
    FakeTaskClock* release_with_;
    std::future<void> future_;
    std::thread thread_;
};

}  // namespace

TEST_CASE("TaskClock fake - advance wakes due waiters only", "[util][taskclock][unit]") {
    FakeTaskClock fake;
    TaskClock& clock = fake;

    Waiter first(clock, 100ms);
    Waiter second(clock, 200ms);
    REQUIRE(fake.wait_for_waiters(2, kBound));
    CHECK(fake.waiter_count() == 2);

    fake.advance(150ms);
    REQUIRE(first.finished_within(kBound));
    CHECK_FALSE(first.result());
    CHECK_FALSE(second.finished_within(50ms));
    CHECK(fake.wait_for_waiters(1, kBound));
    CHECK(fake.waiter_count() == 1);

    // 150 + 50 == the second waiter's deadline: a waiter whose deadline
    // equals the new time is due.
    fake.advance(50ms);
    REQUIRE(second.finished_within(kBound));
    CHECK_FALSE(second.result());
    CHECK(fake.waiter_count() == 0);
    CHECK(clock.now() == TaskClock::clock::time_point{} + 200ms);
}

TEST_CASE("TaskClock fake - cancel wins", "[util][taskclock][unit]") {
    FakeTaskClock fake;
    TaskClock& clock = fake;

    SECTION("a notify with the flag set releases the waiter before its deadline") {
        Waiter waiter(clock, 100ms);
        REQUIRE(fake.wait_for_waiters(1, kBound));
        waiter.cancel(true);
        REQUIRE(waiter.finished_within(kBound));
        CHECK(waiter.result());
        CHECK(clock.now() == TaskClock::clock::time_point{});
        CHECK(fake.waiter_count() == 0);
    }

    SECTION("a flag set before the deadline passes still reports true") {
        Waiter waiter(clock, 100ms);
        REQUIRE(fake.wait_for_waiters(1, kBound));
        waiter.cancel(false);
        fake.advance(200ms);
        REQUIRE(waiter.finished_within(kBound));
        CHECK(waiter.result());
    }

    SECTION("a predicate already true returns true without registering") {
        std::atomic<bool> hook_called{false};
        fake.before_block = [&hook_called] { hook_called = true; };
        auto result = std::async(std::launch::async, [&clock, &fake] {
            std::mutex mutex;
            std::condition_variable cv;
            std::unique_lock<std::mutex> lock(mutex);
            std::size_t count_in_pred = 99;
            bool r = clock.wait_for(lock, cv, 100ms, [&] {
                count_in_pred = fake.waiter_count();
                return true;
            });
            return r && count_in_pred == 0;
        });
        REQUIRE(result.wait_for(kBound) == std::future_status::ready);
        CHECK(result.get());
        CHECK_FALSE(hook_called.load());
        CHECK(fake.waiter_count() == 0);
        fake.before_block = nullptr;
    }

    SECTION("a timeout of zero or less evaluates the predicate once and returns") {
        auto result = std::async(std::launch::async, [&clock] {
            std::mutex mutex;
            std::condition_variable cv;
            std::unique_lock<std::mutex> lock(mutex);
            int calls = 0;
            bool zero = clock.wait_for(lock, cv, 0ns, [&calls] {
                ++calls;
                return false;
            });
            bool negative = clock.wait_for(lock, cv, -5ms, [&calls] {
                ++calls;
                return false;
            });
            return !zero && !negative && calls == 2;
        });
        REQUIRE(result.wait_for(kBound) == std::future_status::ready);
        CHECK(result.get());
        CHECK(fake.waiter_count() == 0);
    }

    SECTION("a timeout too large for the clock waits until the flag is set") {
        Waiter waiter(clock, std::chrono::nanoseconds::max());
        REQUIRE(fake.wait_for_waiters(1, kBound));
        CHECK_FALSE(waiter.finished_within(50ms));
        fake.advance(24h);
        CHECK_FALSE(waiter.finished_within(50ms));
        waiter.cancel(true);
        REQUIRE(waiter.finished_within(kBound));
        CHECK(waiter.result());
    }
}

TEST_CASE("TaskClock fake - sleep_for and the waiter rendezvous", "[util][taskclock][unit]") {
    FakeTaskClock fake;
    TaskClock& clock = fake;

    SECTION("a sleeper is a waiter and wakes exactly at its deadline") {
        Sleeper sleeper(clock, 1s, &fake);
        REQUIRE(fake.wait_for_waiters(1, kBound));
        fake.advance(999ms);
        CHECK_FALSE(sleeper.finished_within(50ms));
        CHECK(fake.waiter_count() == 1);
        fake.advance(1ms);
        REQUIRE(sleeper.finished_within(kBound));
        CHECK(fake.waiter_count() == 0);
    }

    SECTION("destroying a sleeper that has not reached sleep_for yet is bounded") {
        // The thread is held off sleep_for until the destructor has moved
        // the time, so it registers with a deadline past that advance.
        auto sleeper = std::make_unique<Sleeper>(clock, 1s, &fake, [&clock] {
            const auto give_up = std::chrono::steady_clock::now() + kBound;
            while (clock.now() == TaskClock::clock::time_point{} && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(1ms);
            }
        });
        auto destroyed = std::make_shared<std::promise<void>>();
        auto destroyed_future = destroyed->get_future();
        std::thread destroyer([&sleeper, destroyed] {
            sleeper.reset();
            destroyed->set_value();
        });
        const bool bounded = destroyed_future.wait_for(2 * kBound) == std::future_status::ready;
        CHECK(bounded);
        // On failure, release the stuck sleeper so the case still tears down.
        for (int i = 0; i < 100 && destroyed_future.wait_for(10ms) != std::future_status::ready; ++i) {
            fake.advance(24h);
        }
        destroyer.join();
    }

    SECTION("wait_for_waiters with no waiter times out in real time") {
        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(fake.wait_for_waiters(1, 50ms));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(elapsed >= 45ms);
        CHECK(elapsed < kBound);
    }

    SECTION("advance refuses a negative step") {
        CHECK_THROWS_AS(fake.advance(-1ms), std::invalid_argument);
        CHECK(clock.now() == TaskClock::clock::time_point{});
    }
}

TEST_CASE("TaskClock fake - wait_count grows per registration and advance reports the waiters it woke",
          "[util][taskclock][unit]") {
    // open-astro#743: wait_count() is the cumulative registration count;
    // advance() says how many wait_for waiters it notified. A later case
    // pins wait_for_woken_settled(), which a test stepping a driver through
    // consecutive waits uses instead of the two.
    FakeTaskClock clock;
    CHECK(clock.wait_count() == 0);
    CHECK(clock.advance(1s) == 0);

    Waiter first(clock, 100ms);
    REQUIRE(clock.wait_for_wait_count(1, kBound));
    Waiter second(clock, 300ms);
    REQUIRE(clock.wait_for_wait_count(2, kBound));
    CHECK(clock.waiter_count() == 2);

    CHECK(clock.advance(100ms) == 1);  // only the first is due
    REQUIRE(first.finished_within(kBound));
    CHECK_FALSE(first.result());
    CHECK(clock.wait_count() == 2);  // a wake is not a registration
    CHECK(clock.advance(200ms) == 1);
    REQUIRE(second.finished_within(kBound));
    CHECK_FALSE(second.result());
    CHECK(clock.advance(1s) == 0);

    // A sleeper counts as a registration too.
    std::thread sleeper([&] { clock.sleep_for(10ms); });
    REQUIRE(clock.wait_for_wait_count(3, kBound));
    CHECK(clock.advance(10ms) == 0);  // sleepers are not reported
    sleeper.join();
    CHECK_FALSE(clock.wait_for_wait_count(4, 50ms));
}

TEST_CASE("TaskClock fake - advance does not count a due waiter that left before it was notified",
          "[util][taskclock][unit]") {
    // advance() lists the due waiters under its own mutex, then visits each
    // under that waiter's mutex. A waiter cancelled in between is skipped,
    // and a caller that waits for the reported number of re-registrations
    // must not wait for it (open-astro#743).
    FakeTaskClock fake;
    TaskClock& clock = fake;
    Waiter first(clock, 100ms);  // registered first, so visited first
    REQUIRE(fake.wait_for_waiters(1, kBound));
    Waiter second(clock, 100ms);
    REQUIRE(fake.wait_for_waiters(2, kBound));

    std::future<std::size_t> woken;
    {
        // Holding the first waiter's mutex parks advance() at its first
        // visit, after it has moved the time and listed both waiters.
        std::unique_lock<std::mutex> hold(first.mutex());
        woken = std::async(std::launch::async, [&fake] { return fake.advance(100ms); });
        const auto give_up = std::chrono::steady_clock::now() + kBound;
        while (clock.now() == TaskClock::clock::time_point{} && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(clock.now() == TaskClock::clock::time_point{} + 100ms);
        second.cancel(true);
        REQUIRE(second.finished_within(kBound));
        CHECK(second.result());
    }
    REQUIRE(woken.wait_for(kBound) == std::future_status::ready);
    CHECK(woken.get() == 1);  // the second left before its visit
    REQUIRE(first.finished_within(kBound));
    CHECK_FALSE(first.result());
}

TEST_CASE("TaskClock fake - wait_for_woken_settled waits until each woken thread waits again or exits",
          "[util][taskclock][unit]") {
    // open-astro#743: a test stepping a task through consecutive waits must
    // not advance() again while the task it woke is between two waits.
    FakeTaskClock fake;
    TaskClock& clock = fake;
    CHECK(fake.wait_for_woken_settled(0ms));  // nothing woken yet

    std::promise<void> release;
    std::shared_future<void> between = release.get_future().share();
    auto wait_once = [&clock] {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        clock.wait_for(lock, cv, 100ms, [] { return false; });
    };

    SECTION("a woken thread that waits again") {
        std::thread task([&] {
            wait_once();
            between.wait();  // between its two waits
            wait_once();
        });
        REQUIRE(fake.wait_for_waiters(1, kBound));
        CHECK(fake.advance(100ms) == 1);
        CHECK_FALSE(fake.wait_for_woken_settled(50ms));
        release.set_value();
        CHECK(fake.wait_for_woken_settled(kBound));
        CHECK(fake.wait_for_waiters(1, kBound));
        fake.advance(100ms);
        task.join();
    }

    SECTION("a woken thread that exits") {
        std::thread task([&] {
            wait_once();
            between.wait();
        });
        REQUIRE(fake.wait_for_waiters(1, kBound));
        CHECK(fake.advance(100ms) == 1);
        CHECK_FALSE(fake.wait_for_woken_settled(50ms));
        release.set_value();
        CHECK(fake.wait_for_woken_settled(kBound));
        task.join();
    }

    SECTION("a woken sleeper that exits") {
        std::thread task([&] {
            clock.sleep_for(10ms);
            between.wait();
        });
        REQUIRE(fake.wait_for_waiters(1, kBound));
        CHECK(fake.advance(10ms) == 0);  // sleepers are not counted, but are tracked
        CHECK_FALSE(fake.wait_for_woken_settled(50ms));
        release.set_value();
        CHECK(fake.wait_for_woken_settled(kBound));
        task.join();
    }

    SECTION("a due waiter that leaves before advance() reaches it") {
        // advance() moves the time, then visits the due waiters one by one.
        // A waiter that leaves on its own in between is not notified, but it
        // is still between two waits and must be tracked.
        Waiter first(clock, 100ms);  // registered first, so visited first
        REQUIRE(fake.wait_for_waiters(1, kBound));
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> left{false};
        std::thread task([&] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                clock.wait_for(lock, cv, 100ms, [] { return false; });
            }
            left = true;
            between.wait();
        });
        REQUIRE(fake.wait_for_waiters(2, kBound));

        std::future<std::size_t> woken;
        {
            // Holding the first waiter's mutex parks advance() at its first
            // visit, after it has moved the time.
            std::unique_lock<std::mutex> hold(first.mutex());
            woken = std::async(std::launch::async, [&fake] { return fake.advance(100ms); });
            const auto give_up = std::chrono::steady_clock::now() + kBound;
            while (clock.now() == TaskClock::clock::time_point{} && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(1ms);
            }
            REQUIRE(clock.now() == TaskClock::clock::time_point{} + 100ms);
            {
                std::lock_guard<std::mutex> lock(mutex);
                cv.notify_all();  // the task sees its deadline reached and leaves
            }
            while (!left && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(1ms);
            }
            REQUIRE(left);
        }
        REQUIRE(woken.wait_for(kBound) == std::future_status::ready);
        CHECK(woken.get() == 1);  // only the first was notified
        REQUIRE(first.finished_within(kBound));
        CHECK_FALSE(fake.wait_for_woken_settled(50ms));  // the task is between waits
        release.set_value();
        CHECK(fake.wait_for_woken_settled(kBound));
        task.join();
    }
}

TEST_CASE("TaskClock fake - advance cannot lose a wakeup between check and block", "[util][taskclock][stress-guard]") {
    FakeTaskClock fake;
    TaskClock& clock = fake;

    SECTION("advance inside the window between the predicate check and the block") {
        // The hook runs with the waiter's mutex held, after its checks and
        // before cv.wait. It tells the test thread the window is open, then
        // holds the window open for ~50 ms of real time while the test
        // thread advances past the deadline. advance() must take the
        // waiter's mutex before notifying, so the notify lands only once the
        // waiter is really blocked.
        std::promise<void> in_window;
        std::atomic<bool> fired{false};
        fake.before_block = [&in_window, &fired] {
            if (!fired.exchange(true)) {
                in_window.set_value();
                std::this_thread::sleep_for(50ms);
            }
        };
        {
            Waiter waiter(clock, 100ms);
            REQUIRE(in_window.get_future().wait_for(kBound) == std::future_status::ready);
            fake.advance(200ms);
            const bool woke = waiter.finished_within(kBound);
            CHECK(woke);  // a missed wakeup leaves the waiter blocked here
            if (woke) {
                CHECK_FALSE(waiter.result());
            }
        }  // ~Waiter cancels and joins, so a red run still tears down
        fake.before_block = nullptr;
    }

    SECTION("1000 advances raced against a fresh waiter") {
        // No hook: advance() runs at whatever point the waiter has reached,
        // before registration, between check and block, or while blocked.
        // Under TSan this must finish with no report.
        int finished = 0;
        for (int i = 0; i < 1000; ++i) {
            Waiter waiter(clock, 1ms);
            const auto give_up = std::chrono::steady_clock::now() + kBound;
            while (!waiter.finished_within(0ms) && std::chrono::steady_clock::now() < give_up) {
                fake.advance(1ms);
            }
            if (waiter.finished_within(0ms)) {
                ++finished;
                CHECK_FALSE(waiter.result());
            }
        }
        CHECK(finished == 1000);
        CHECK(fake.waiter_count() == 0);
    }
}

TEST_CASE("TaskClock real adapter - steady clock and condition variable", "[util][taskclock][unit]") {
    RealTaskClock real;
    TaskClock& clock = real;

    SECTION("now never decreases") {
        auto previous = clock.now();
        bool monotonic = true;
        for (int i = 0; i < 1000; ++i) {
            const auto current = clock.now();
            monotonic = monotonic && current >= previous;
            previous = current;
        }
        CHECK(monotonic);
    }

    SECTION("a predicate already true returns at once") {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        const auto start = std::chrono::steady_clock::now();
        CHECK(clock.wait_for(lock, cv, 10s, [] { return true; }));
        CHECK(std::chrono::steady_clock::now() - start < 1s);
    }

    SECTION("a false predicate times out after the timeout of real time") {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(clock.wait_for(lock, cv, 20ms, [] { return false; }));
        CHECK(std::chrono::steady_clock::now() - start >= 20ms);
    }

    SECTION("a timeout of zero or less evaluates the predicate once") {
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        int calls = 0;
        CHECK_FALSE(clock.wait_for(lock, cv, 0ns, [&calls] {
            ++calls;
            return false;
        }));
        CHECK(calls == 1);
    }

    SECTION("a notify with the flag set returns true before the timeout") {
        const auto start = std::chrono::steady_clock::now();
        Waiter waiter(clock, 10s);
        // Without this rendezvous the flag is usually set before the waiter
        // first locks, and the case passes on the already-true path.
        REQUIRE(waiter.blocked_within(kBound));
        waiter.cancel(true);
        REQUIRE(waiter.finished_within(kBound));
        CHECK(waiter.result());
        CHECK(std::chrono::steady_clock::now() - start < 5s);
    }

    SECTION("a timeout too large for the clock waits until the flag is set") {
        // libstdc++ adds the timeout to steady_clock::now(), which overflows
        // for a timeout near nanoseconds::max() and returned false at once.
        Waiter waiter(clock, std::chrono::nanoseconds::max());
        CHECK_FALSE(waiter.finished_within(50ms));
        waiter.cancel(true);
        REQUIRE(waiter.finished_within(kBound));
        CHECK(waiter.result());
    }

    SECTION("sleep_for sleeps real time") {
        const auto start = std::chrono::steady_clock::now();
        clock.sleep_for(5ms);
        CHECK(std::chrono::steady_clock::now() - start >= 5ms);
    }

    SECTION("default_task_clock is one process-wide object") {
        TaskClock& a = default_task_clock();
        TaskClock& b = default_task_clock();
        CHECK(&a == &b);
        CHECK(dynamic_cast<RealTaskClock*>(&a) != nullptr);
    }
}
