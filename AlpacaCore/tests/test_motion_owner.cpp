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

// Tests for util::MotionOwner, the per-axis motion owner (#2263): which axes a
// motion command claims, and what a new command does to a body already running.
// The policy is a table of (running command, new command) to outcome, held here
// as data so the rule reads in one place. The last case runs a body in a
// util::AsyncOperation slot on a FakeTaskClock, the way an adopter uses it.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/motion_owner.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::test::FakeTaskClock;
using alpacacore::util::AsyncOperation;
using alpacacore::util::MotionClaim;
using alpacacore::util::MotionCommand;
using alpacacore::util::MotionOwner;
using alpacacore::util::OperationContext;
using alpacacore::util::OperationGeneration;
using alpacacore::util::StopReason;
using namespace std::chrono_literals;

namespace {

// What the running body sees after the new command, or that the new command was refused.
enum class Outcome : std::uint8_t { Kept, Superseded, Refused };

struct Row {
    MotionCommand running;
    int running_axis;
    MotionCommand next;
    int next_axis;
    Outcome outcome;
};

// The #2263 policy table. Slew stands for every command that moves both axes: a slew, Park or FindHome.
// Axis 0 is the primary axis (RA or azimuth), axis 1 the secondary (Dec or altitude).
constexpr Row kPolicy[] = {
    // A slew, park or home claims both axes: it supersedes everything.
    {MotionCommand::Slew, 0, MotionCommand::Slew, 0, Outcome::Superseded},
    {MotionCommand::PulseGuide, 0, MotionCommand::Slew, 0, Outcome::Superseded},
    {MotionCommand::PulseGuide, 1, MotionCommand::Slew, 0, Outcome::Superseded},
    {MotionCommand::MoveAxis, 0, MotionCommand::Slew, 0, Outcome::Superseded},
    {MotionCommand::MoveAxis, 1, MotionCommand::Slew, 0, Outcome::Superseded},
    // A pulse claims its own axis and leaves the other alone.
    {MotionCommand::PulseGuide, 0, MotionCommand::PulseGuide, 0, Outcome::Superseded},
    {MotionCommand::PulseGuide, 1, MotionCommand::PulseGuide, 0, Outcome::Kept},
    {MotionCommand::MoveAxis, 0, MotionCommand::PulseGuide, 0, Outcome::Superseded},
    {MotionCommand::MoveAxis, 1, MotionCommand::PulseGuide, 0, Outcome::Kept},
    // A PulseGuide while a slew, park or home runs is refused and changes nothing.
    {MotionCommand::Slew, 0, MotionCommand::PulseGuide, 0, Outcome::Refused},
    {MotionCommand::Slew, 0, MotionCommand::PulseGuide, 1, Outcome::Refused},
    // A non-zero MoveAxis claims its own axis: it supersedes a pulse on that axis, keeps one on the other, and
    // supersedes a slew, park or home (those own both axes).
    {MotionCommand::PulseGuide, 0, MotionCommand::MoveAxis, 0, Outcome::Superseded},
    {MotionCommand::PulseGuide, 0, MotionCommand::MoveAxis, 1, Outcome::Kept},
    {MotionCommand::MoveAxis, 0, MotionCommand::MoveAxis, 0, Outcome::Superseded},
    {MotionCommand::MoveAxis, 0, MotionCommand::MoveAxis, 1, Outcome::Kept},
    {MotionCommand::Slew, 0, MotionCommand::MoveAxis, 0, Outcome::Superseded},
    {MotionCommand::Slew, 0, MotionCommand::MoveAxis, 1, Outcome::Superseded},
};

const char* name(MotionCommand c) {
    switch (c) {
        case MotionCommand::Slew:
            return "Slew";
        case MotionCommand::PulseGuide:
            return "PulseGuide";
        case MotionCommand::MoveAxis:
            return "MoveAxis";
    }
    return "?";
}

}  // namespace

TEST_CASE("MotionOwner - a new command supersedes, keeps or is refused per the policy table",
          "[util][motion_owner][unit]") {
    for (const Row& row : kPolicy) {
        INFO(std::string(name(row.running)) + " on axis " + std::to_string(row.running_axis) + ", then " +
             name(row.next) + " on axis " + std::to_string(row.next_axis));
        MotionOwner owner;
        const MotionClaim running = owner.claim(row.running, row.running_axis);
        if (row.outcome == Outcome::Refused) {
            try {
                (void)owner.claim(row.next, row.next_axis);
                FAIL("the new command was not refused");
            } catch (const AlpacaException& e) {
                CHECK(e.error_code() == AlpacaError::InvalidOperation);
            }
            CHECK(owner.stop_reason(running) == StopReason::None);
            CHECK(owner.slew_running());
            continue;
        }
        const MotionClaim next = owner.claim(row.next, row.next_axis);
        CHECK(owner.stop_reason(running) ==
              (row.outcome == Outcome::Superseded ? StopReason::Superseded : StopReason::None));
        CHECK(owner.stop_reason(next) == StopReason::None);
    }
}

TEST_CASE("MotionOwner - a stop cancels the bodies on its axes only", "[util][motion_owner][unit]") {
    MotionOwner owner;
    const MotionClaim ra = owner.claim(MotionCommand::PulseGuide, 0);
    const MotionClaim dec = owner.claim(MotionCommand::MoveAxis, 1);

    // MoveAxis(0, 0) cancels its own axis.
    owner.cancel(MotionOwner::axes_of(MotionCommand::MoveAxis, 0));
    CHECK(owner.stop_reason(ra) == StopReason::Cancelled);
    CHECK(owner.stop_reason(dec) == StopReason::None);

    // AbortSlew and Halt cancel both axes.
    owner.cancel(MotionOwner::kBothAxes);
    CHECK(owner.stop_reason(dec) == StopReason::Cancelled);

    // A slew owns both axes, so a stop on either axis reaches it.
    const MotionClaim slew = owner.claim(MotionCommand::Slew);
    owner.cancel(MotionOwner::axes_of(MotionCommand::MoveAxis, 1));
    CHECK(owner.stop_reason(slew) == StopReason::Cancelled);

    // Once the cancelled slew's body finishes, a command claimed after the stop starts clean.
    owner.finished(slew);
    const MotionClaim after = owner.claim(MotionCommand::PulseGuide, 1);
    CHECK(owner.stop_reason(after) == StopReason::None);
}

TEST_CASE("MotionOwner - superseded wins over cancelled and both are sticky", "[util][motion_owner][unit]") {
    MotionOwner owner;
    const MotionClaim pulse = owner.claim(MotionCommand::PulseGuide, 1);
    owner.cancel(MotionOwner::kBothAxes);
    CHECK(owner.stop_reason(pulse) == StopReason::Cancelled);

    // Decision 0006 rule 6: once a newer command owns the axis, the old body sends it nothing, even after a cancel.
    (void)owner.claim(MotionCommand::MoveAxis, 1);
    CHECK(owner.stop_reason(pulse) == StopReason::Superseded);

    // Later claims on the other axis and later stops do not turn it back.
    (void)owner.claim(MotionCommand::PulseGuide, 0);
    owner.cancel(MotionOwner::kBothAxes);
    CHECK(owner.stop_reason(pulse) == StopReason::Superseded);
}

TEST_CASE("MotionOwner - PulseGuide is refused until the slew finishes or loses both axes",
          "[util][motion_owner][unit]") {
    MotionOwner owner;
    CHECK_FALSE(owner.slew_running());

    SECTION("finished") {
        const MotionClaim slew = owner.claim(MotionCommand::Slew);
        CHECK(owner.slew_running());
        owner.finished(slew);
        CHECK_FALSE(owner.slew_running());
        CHECK(owner.stop_reason(owner.claim(MotionCommand::PulseGuide, 0)) == StopReason::None);
    }
    SECTION("a cancelled slew still runs until it finishes") {
        const MotionClaim slew = owner.claim(MotionCommand::Slew);
        owner.cancel(MotionOwner::kBothAxes);
        CHECK(owner.slew_running());
        owner.finished(slew);
        CHECK_FALSE(owner.slew_running());
    }
    SECTION("superseded by a MoveAxis") {
        (void)owner.claim(MotionCommand::Slew);
        (void)owner.claim(MotionCommand::MoveAxis, 1);
        CHECK_FALSE(owner.slew_running());
    }
    SECTION("an old slew's finish does not end the newer one") {
        const MotionClaim first = owner.claim(MotionCommand::Slew);
        (void)owner.claim(MotionCommand::Slew);
        owner.finished(first);
        CHECK(owner.slew_running());
    }
    SECTION("a refused PulseGuide claims nothing") {
        const MotionClaim slew = owner.claim(MotionCommand::Slew);
        CHECK_THROWS(owner.claim(MotionCommand::PulseGuide, 0));
        CHECK(owner.stop_reason(slew) == StopReason::None);
        CHECK(owner.slew_running());
    }
}

TEST_CASE("MotionOwner - axes_of names the axes each command claims", "[util][motion_owner][unit]") {
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::Slew, 0) == MotionOwner::kBothAxes);
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::Slew, 1) == MotionOwner::kBothAxes);
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::PulseGuide, 0) == MotionOwner::kPrimaryAxis);
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::PulseGuide, 1) == MotionOwner::kSecondaryAxis);
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::MoveAxis, 0) == MotionOwner::kPrimaryAxis);
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::MoveAxis, 1) == MotionOwner::kSecondaryAxis);
    // The tertiary axis is neither: a MoveAxis on it leaves a Dec pulse running.
    STATIC_REQUIRE(MotionOwner::axes_of(MotionCommand::MoveAxis, 2) == 0);
    MotionOwner owner;
    const MotionClaim dec = owner.claim(MotionCommand::PulseGuide, 1);
    (void)owner.claim(MotionCommand::MoveAxis, 2);
    CHECK(owner.stop_reason(dec) == StopReason::None);
}

TEST_CASE("MotionOwner - a body in a slot on the task clock sees its own reason", "[util][motion_owner][unit]") {
    FakeTaskClock clock;
    OperationGeneration slot_generation;
    MotionOwner owner;
    std::atomic<int> checks{0};
    std::atomic<int> reason{-1};

    // The driver claims under its mutex_ before it starts the body (decision 0006 rule 3); the body checks the
    // owner after each wait, as an adopter's body checks before it touches the mount.
    const MotionClaim pulse = owner.claim(MotionCommand::PulseGuide, 1);
    {
        AsyncOperation slot("pulse", slot_generation, clock);
        slot.start([&](OperationContext& ctx) {
            for (;;) {
                const bool more = ctx.wait_for(100ms);
                const StopReason r = owner.stop_reason(pulse);
                ++checks;
                if (r != StopReason::None) {
                    reason = static_cast<int>(r);
                    return;
                }
                if (!more) {
                    return;
                }
            }
        });
        REQUIRE(clock.wait_for_waiters(1, std::chrono::milliseconds(2000)));
        (void)owner.claim(MotionCommand::MoveAxis, 0);  // the other axis: the pulse keeps running
        clock.advance(100ms);
        const auto give_up = std::chrono::steady_clock::now() + 2s;
        while (checks == 0 && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::yield();
        }
        REQUIRE(checks >= 1);
        CHECK(reason == -1);
        (void)owner.claim(MotionCommand::MoveAxis, 1);  // its own axis: superseded
        // Wakes the body; it reads the owner once more.
        slot.cancel_all_and_join();
    }
    CHECK(reason == static_cast<int>(StopReason::Superseded));
}
