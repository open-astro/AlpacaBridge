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

// PolledState (decision records 0009, 0010 and 0016) on the virtual
// FakeTaskClock with a scripted probe: no real sleeps.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/polled_state.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::test::FakeTaskClock;
using alpacacore::util::OperationGeneration;
using alpacacore::util::PolledState;
using namespace std::chrono_literals;

namespace {

constexpr auto kBound = 2000ms;
constexpr auto kPeriod = 100ms;

struct Frame {
    int value = 0;
};

// A scripted probe: `fail` makes the exchange throw, `next` is the frame it
// measures. `during` runs inside the exchange (a write landing mid-probe, a
// slow exchange on the virtual clock).
struct Rig {
    FakeTaskClock clock;
    OperationGeneration generation;
    std::atomic<bool> fail{false};
    std::atomic<int> next{1};
    std::atomic<int> probes{0};
    std::atomic<int> lost_calls{0};
    std::atomic<std::thread::id> lost_thread{};
    std::function<void()> during;
    std::unique_ptr<PolledState<Frame>> state;

    explicit Rig(int threshold = 3, std::chrono::nanoseconds max_age = 2 * kPeriod) {
        PolledState<Frame>::Config config;
        config.period = kPeriod;
        config.fault_threshold = threshold;
        config.max_age = max_age;
        config.log_tag = "Test";
        state = std::make_unique<PolledState<Frame>>(
            clock, generation, config,
            [this] {
                ++probes;
                if (during) during();
                if (fail.load()) throw std::runtime_error("no reply");
                return Frame{next.load()};
            },
            [this] {
                lost_thread.store(std::this_thread::get_id());
                ++lost_calls;
            });
    }

    // One tick of the poll thread: it parked, the clock moves, it probed and parked again.
    void tick() {
        clock.advance(kPeriod);
        REQUIRE(clock.wait_for_woken_settled(kBound));
        REQUIRE(clock.wait_for_waiters(1, kBound));
    }
};

}  // namespace

// Falsified by: polled_state.h:start() skip the second poll_once() when nothing published.
TEST_CASE("PolledState - start probes inline, retries once, and reports whether a frame is held",
          "[polled_state][unit]") {
    Rig rig;
    rig.fail = true;
    CHECK_FALSE(rig.state->start());
    CHECK(rig.probes.load() == 2);
    CHECK(rig.clock.waiter_count() == 0);  // no frame: the slot never started
    CHECK_FALSE(rig.state->read().has_value());

    rig.fail = false;
    rig.next = 7;
    CHECK(rig.state->start());
    CHECK(rig.probes.load() == 3);  // one inline probe, no retry once a frame is held
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    CHECK(rig.state->served().value.value == 7);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:run() replace `ticks * period - spent` with `period` (no start-to-start cadence).
TEST_CASE("PolledState - polls start to start and skips the ticks a slow probe ran over", "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    CHECK(rig.probes.load() == 1);

    rig.tick();
    CHECK(rig.probes.load() == 2);  // one probe per period

    // A probe that takes 250 ms covers two further ticks; the next one is due
    // 50 ms later (3 periods from the start), with no catch-up burst.
    rig.during = [&rig] { rig.clock.advance(250ms); };
    rig.tick();
    CHECK(rig.probes.load() == 3);
    rig.during = nullptr;
    rig.clock.advance(49ms);
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    CHECK(rig.probes.load() == 3);
    rig.clock.advance(1ms);
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    CHECK(rig.probes.load() == 4);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:poll_once() publish with a fresh begin_poll() taken after the probe.
TEST_CASE("PolledState - a write drops the poll in flight, the next poll publishes", "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.next = 5;
    rig.during = [&rig] { rig.state->write([](Frame& f) { f.value = 99; }); };
    rig.state->poll_now_for_testing();
    CHECK(rig.state->served().value.value == 99);  // the write-through stands; the sampled 5 was dropped
    rig.during = nullptr;
    rig.state->poll_now_for_testing();
    CHECK(rig.state->served().value.value == 5);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:note_failure() latch on fault_threshold + 1 failures.
TEST_CASE("PolledState - N failed probes latch a fault, the next good probe clears it", "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.next = 2;
    rig.fail = true;
    for (int i = 0; i < 2; ++i) {
        rig.state->poll_now_for_testing();
        CHECK(rig.state->served().value.value == 1);  // below the threshold the last frame is served
        CHECK(rig.state->fault().empty());
    }
    rig.state->poll_now_for_testing();
    CHECK_FALSE(rig.state->fault().empty());
    try {
        (void)rig.state->served();
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
        CHECK(std::string(ex.what()).find("3 consecutive failures") != std::string::npos);
    }
    CHECK(rig.state->read().has_value());  // the latch refuses the cache, it does not drop it
    CHECK_FALSE(rig.state->link_lost());

    const int before = rig.probes.load();
    rig.fail = false;
    rig.state->poll_now_for_testing();  // the probe keeps running while latched
    CHECK(rig.probes.load() == before + 1);
    CHECK(rig.state->fault().empty());
    CHECK(rig.state->served().value.value == 2);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:cycle() drop the lost_notified_ exchange (on_lost() every cycle).
TEST_CASE("PolledState - a fault stale past the bound sets lost and calls on_lost exactly once",
          "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.fail = true;
    for (int i = 0; i < 3; ++i) rig.state->poll_now_for_testing();
    REQUIRE_FALSE(rig.state->fault().empty());
    CHECK_FALSE(rig.state->link_lost());

    rig.clock.advance(alpacacore::util::kLinkStalenessBound - 1ms);
    rig.state->poll_now_for_testing();
    CHECK_FALSE(rig.state->link_lost());
    CHECK(rig.lost_calls.load() == 0);

    rig.clock.advance(1ms);
    rig.state->poll_now_for_testing();
    CHECK(rig.state->link_lost());
    CHECK(rig.lost_calls.load() == 1);
    rig.state->poll_now_for_testing();
    rig.state->poll_now_for_testing();
    CHECK(rig.lost_calls.load() == 1);
    CHECK_FALSE(rig.state->fault().empty());  // kept, so a listing says why
    try {
        (void)rig.state->served();
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:note_failure() delete the `reading ... >= kLinkStalenessBound` clause.
TEST_CASE("PolledState - a frame older than the bound sets lost with no fault latched", "[polled_state][unit]") {
    Rig rig(1000);  // a threshold the failures never reach
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.fail = true;
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    rig.state->poll_now_for_testing();
    CHECK(rig.state->fault().empty());
    CHECK(rig.state->link_lost());
    CHECK(rig.lost_calls.load() == 1);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:served() throw when `reading->stale`.
TEST_CASE("PolledState - served rules: stale is served, faulted and lost throw, nothing published is NotConnected",
          "[polled_state][unit]") {
    Rig rig;
    try {
        (void)rig.state->served();  // before the first frame
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
    CHECK_FALSE(rig.state->age().has_value());

    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.clock.advance(300ms);  // past max_age (200 ms), far from the bound
    const auto stale = rig.state->served();
    CHECK(stale.stale);
    CHECK(stale.value.value == 1);
    REQUIRE(rig.state->age().has_value());
    CHECK(*rig.state->age() == 300ms);

    // A getter that finds the frame older than the bound latches the loss; the
    // transport release stays with the poll thread.
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    try {
        (void)rig.state->served();
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
    CHECK(rig.state->link_lost());
    CHECK(rig.lost_calls.load() == 0);
    rig.state->poll_now_for_testing();
    CHECK(rig.lost_calls.load() == 1);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:reset() delete `lost_notified_.store(false)`.
TEST_CASE("PolledState - reset clears the frame, the fault and the loss latch", "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.fail = true;
    for (int i = 0; i < 3; ++i) rig.state->poll_now_for_testing();
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    rig.state->poll_now_for_testing();
    REQUIRE(rig.state->link_lost());
    REQUIRE(rig.lost_calls.load() == 1);

    rig.state->stop_and_join();
    rig.state->reset();
    CHECK_FALSE(rig.state->read().has_value());
    CHECK(rig.state->fault().empty());
    CHECK_FALSE(rig.state->link_lost());

    // A second outage after a reconnect calls on_lost() again.
    rig.fail = false;
    REQUIRE(rig.state->start());
    rig.state->pause_for_testing(true);
    rig.fail = true;
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    rig.state->poll_now_for_testing();
    CHECK(rig.lost_calls.load() == 2);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:run() ignore paused_ (probe while paused).
TEST_CASE("PolledState - pause_for_testing holds the poll thread, poll_now_for_testing runs one cycle",
          "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    rig.state->pause_for_testing(true);
    rig.clock.advance(kPeriod);
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    CHECK(rig.probes.load() == 1);  // the tick passed, nothing was probed

    rig.next = 4;
    rig.state->poll_now_for_testing();
    CHECK(rig.probes.load() == 2);
    CHECK(rig.state->served().value.value == 4);

    rig.state->pause_for_testing(false);
    rig.tick();
    CHECK(rig.probes.load() == 3);
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:stop_and_join() remove the cancel_all_and_join() call.
TEST_CASE("PolledState - stop_and_join returns with the poll parked on the clock and ends polling",
          "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    rig.state->stop_and_join();
    CHECK(rig.clock.waiter_count() == 0);
    const int probes = rig.probes.load();
    rig.clock.advance(1s);
    CHECK(rig.probes.load() == probes);
}

// Falsified by: polled_state.h:note_failure() delete the frame-age clause (the poll never ends with the loss).
TEST_CASE("PolledState - on_lost runs on the poll thread and the poll ends with the loss", "[polled_state][unit]") {
    Rig rig;
    REQUIRE(rig.state->start());
    REQUIRE(rig.clock.wait_for_waiters(1, kBound));
    rig.fail = true;
    rig.tick();
    rig.clock.advance(alpacacore::util::kLinkStalenessBound);
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    const auto deadline = std::chrono::steady_clock::now() + kBound;
    while (rig.clock.waiter_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(rig.clock.waiter_count() == 0);
    CHECK(rig.lost_calls.load() == 1);
    CHECK(rig.lost_thread.load() != std::this_thread::get_id());
    CHECK(rig.state->link_lost());
    rig.state->stop_and_join();
}

// Falsified by: polled_state.h:start() return true after the slot's start() throws.
TEST_CASE("PolledState - start reports false when the poll thread cannot start", "[polled_state][unit]") {
    Rig rig;
    rig.state->set_spawn_for_testing(
        [](std::function<void()>) -> std::thread { throw std::system_error(EAGAIN, std::generic_category()); });
    CHECK_FALSE(rig.state->start());
    CHECK(rig.clock.waiter_count() == 0);
}

// Falsified by: polled_state.h constructor delete the max_age check.
TEST_CASE("PolledState - max_age must undercut the link staleness bound", "[polled_state][unit]") {
    FakeTaskClock clock;
    OperationGeneration generation;
    PolledState<Frame>::Config config;
    config.max_age = alpacacore::util::kLinkStalenessBound;
    config.log_tag = "Test";
    CHECK_THROWS_AS(PolledState<Frame>(clock, generation, config, [] { return Frame{}; }, [] {}), AlpacaException);
}
