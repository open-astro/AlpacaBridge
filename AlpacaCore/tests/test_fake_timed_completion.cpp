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

// FakeTimedCompletion (#2693): the timed completion the QHY, Player One and SVBONY camera fakes share. Each case
// drives it on a FakeTaskClock and never sleeps.

#include <atomic>
#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "fake_task_clock.h"
#include "fake_timed_completion.h"

namespace {

using alpacacore::test::FakeTaskClock;
using alpacacore::test::FakeTimedCompletion;
using namespace std::chrono_literals;

}  // namespace

TEST_CASE("FakeTimedCompletion - an exposure is not ready before its duration and is after it",
          "[fake][timed-completion][unit]") {
    FakeTimedCompletion timing;
    timing.start(2s);
    CHECK(timing.ready());  // no clock: at once, the unit default

    FakeTaskClock clock;
    timing.set_clock(&clock);
    timing.start(2s);
    CHECK_FALSE(timing.ready());
    CHECK(timing.remaining() == 2s);
    clock.advance(1999ms);
    CHECK_FALSE(timing.ready());
    CHECK(timing.remaining() == 1ms);
    clock.advance(1ms);
    CHECK(timing.ready());
    CHECK(timing.remaining() == 0ns);

    timing.start(1s);
    timing.clear();
    CHECK(timing.ready());
}

TEST_CASE("FakeTimedCompletion - a guide call returns only after its pulse", "[fake][timed-completion][unit]") {
    FakeTaskClock clock;
    FakeTimedCompletion timing;
    timing.set_clock(&clock);
    std::atomic<bool> returned{false};
    std::jthread guide([&] {  // joins on a failed REQUIRE too
        timing.block_for(500ms);
        returned = true;
    });
    REQUIRE(clock.wait_for_waiter_thread(guide.get_id(), 2000ms));
    clock.advance(499ms);
    // Still parked on the clock: the sleeper leaves the waiter list only once its deadline has passed.
    CHECK(clock.waiter_count() == 1);
    CHECK_FALSE(returned.load());
    clock.advance(1ms);
    guide.join();
    CHECK(returned.load());
}
