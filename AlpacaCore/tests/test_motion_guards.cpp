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

// The shared telescope motion guards (util/motion_guards.h, issue #2405). The decision objects are fed scripted
// readings and take no clock. The runner cases run the guard in a real util::AsyncOperation slot on a FakeTaskClock
// and step virtual time with advance(); nothing sleeps.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/motion_guards.h>
#include <alpacacore/util/motion_policy.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeTaskClock;
using namespace alpacacore::util;
using namespace std::chrono_literals;

namespace {

constexpr double kArcsecToDegrees = 1.0 / 3600.0;

MotionTime at(std::chrono::milliseconds t) { return MotionTime{} + t; }

PositionReading read_at(std::chrono::milliseconds t, double ra_hours, double dec_degrees) {
    return PositionReading{MountPosition{ra_hours, dec_degrees}, at(t)};
}

MotionStatusReading status_at(std::chrono::milliseconds t, bool moving) { return MotionStatusReading{moving, at(t)}; }

// Steps virtual time by `step` each time the slot's body is parked on the clock, until the body returns, then joins
// it. Each step waits for the woken body to park again, so no read lands on a later time than its poll.
void step_until_returned(FakeTaskClock& clock, AsyncOperation& slot,
                         std::chrono::nanoseconds step = kStopVerifyPollInterval) {
    int steps = 0;
    while (slot.running()) {
        REQUIRE(++steps <= 400);
        if (clock.wait_for_waiters(1, 100ms)) {
            clock.advance(step);
            REQUIRE(clock.wait_for_woken_settled(2s));
        }
    }
    slot.cancel_all_and_join();
}

// Runs `body` in a slot on `clock` until it returns. Returns the slot's kept failure.
std::optional<std::string> run_in_slot(FakeTaskClock& clock, const std::function<void(OperationContext&)>& body) {
    OperationGeneration generation;
    AsyncOperation slot("motion-guards-test", generation, clock);
    slot.start(body);
    step_until_returned(clock, slot);
    return slot.last_failure();
}

// A scripted mount for the stop runner: what it reports is a function of the virtual time and of the stops sent.
// Every field is set before the slot starts and read only by the slot's thread until the join.
struct ScriptedMount {
    FakeTaskClock& clock;
    std::atomic<int> stops{0};
    std::function<bool(int stops)> moving = [](int) { return false; };
    std::function<MountPosition(int stops, MotionTime now)> position = [](int, MotionTime) {
        return MountPosition{1.0, 10.0};
    };
    std::function<void(int reads)> on_read = [](int) {};
    int reads = 0;

    StopVerifyHooks hooks() {
        return StopVerifyHooks{
            [this] { ++stops; },
            [this]() -> std::optional<MotionStatusReading> {
                on_read(++reads);
                return MotionStatusReading{moving(stops), clock.now()};
            },
            [this]() -> std::optional<PositionReading> {
                return PositionReading{position(stops, clock.now()), clock.now()};
            },
        };
    }
};

}  // namespace

// ── Verified stop: the decision object ────────────────────────────────────────────────────────────────────────

TEST_CASE("StopVerifier - verified when status is stopped and two reads 500 ms apart agree within 30 arcsec",
          "[motion-guards]") {
    StopVerifier verifier(at(0ms));
    CHECK(verifier.feed(at(250ms), status_at(250ms, false), read_at(250ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(500ms), status_at(500ms, false), read_at(500ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(750ms), status_at(750ms, false), read_at(750ms, 1.0, 10.0 + 10 * kArcsecToDegrees)) ==
          StopStep::Verified);
}

TEST_CASE("StopVerifier - status stopped while the position still drifts more than 30 arcsec is not verified",
          "[motion-guards]") {
    // #2396 rig evidence: the HAE29C reports stopped 1-2 s before the motion ends.
    StopVerifier verifier(at(0ms));
    for (int i = 1; i <= 3; ++i) {
        const auto t = std::chrono::milliseconds(250 * i);
        CHECK(verifier.feed(at(t), status_at(t, false), read_at(t, 1.0, 10.0 + i * 20 * kArcsecToDegrees)) ==
              StopStep::Wait);
    }
}

TEST_CASE("StopVerifier - a moving status is not verified even when the position holds", "[motion-guards]") {
    StopVerifier verifier(at(0ms));
    CHECK(verifier.feed(at(250ms), status_at(250ms, true), read_at(250ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(750ms), status_at(750ms, true), read_at(750ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(800ms), status_at(800ms, false), read_at(800ms, 1.0, 10.0)) == StopStep::Verified);
}

TEST_CASE("StopVerifier - reads 499 ms apart do not verify, 500 ms apart do", "[motion-guards]") {
    StopVerifier verifier(at(0ms));
    CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(599ms), status_at(599ms, false), read_at(599ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(600ms), status_at(600ms, false), read_at(600ms, 1.0, 10.0)) == StopStep::Verified);
}

TEST_CASE("StopVerifier - within 30 arcsec is still, beyond it is motion, on either axis", "[motion-guards]") {
    SECTION("Dec") {
        StopVerifier verifier(at(0ms));
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(600ms), status_at(600ms, false), read_at(600ms, 1.0, 10.0 + 31 * kArcsecToDegrees)) ==
              StopStep::Wait);
        CHECK(verifier.feed(at(1100ms), status_at(1100ms, false), read_at(1100ms, 1.0, 10.0 + 62 * kArcsecToDegrees)) ==
              StopStep::Resend);
    }
    SECTION("RA, as wrapped hours x 15 across 0 h") {
        StopVerifier verifier(at(0ms));
        const double ra_20 = 20.0 / 15.0 / 3600.0;  // 20 arcsec of RA in hours
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 24.0 - ra_20 / 2, 10.0)) ==
              StopStep::Wait);
        CHECK(verifier.feed(at(600ms), status_at(600ms, false), read_at(600ms, ra_20 / 2, 10.0)) == StopStep::Verified);
    }
    SECTION("29.9 arcsec verifies, 30.1 does not") {
        StopVerifier verifier(at(0ms));
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(600ms), status_at(600ms, false), read_at(600ms, 1.0, 10.0 + 30.1 * kArcsecToDegrees)) ==
              StopStep::Wait);
        CHECK(verifier.feed(at(1100ms), status_at(1100ms, false),
                            read_at(1100ms, 1.0, 10.0 + 60.0 * kArcsecToDegrees)) == StopStep::Verified);
    }
}

TEST_CASE("StopVerifier - failed, unknown and stale reads never verify", "[motion-guards]") {
    SECTION("a failed position read") {
        StopVerifier verifier(at(0ms));
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(600ms), status_at(600ms, false), std::nullopt) == StopStep::Wait);
    }
    SECTION("an unknown status") {
        StopVerifier verifier(at(0ms));
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(600ms), std::nullopt, read_at(600ms, 1.0, 10.0)) == StopStep::Wait);
    }
    SECTION("a status measured before the stop was sent") {
        StopVerifier verifier(at(1000ms));
        CHECK(verifier.feed(at(1100ms), status_at(1100ms, false), read_at(1100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(1600ms), status_at(900ms, false), read_at(1600ms, 1.0, 10.0)) == StopStep::Wait);
    }
    SECTION("a position measured before the stop was sent") {
        StopVerifier verifier(at(1000ms));
        CHECK(verifier.feed(at(1100ms), status_at(1100ms, false), read_at(400ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(1500ms), status_at(1500ms, false), read_at(1500ms, 1.0, 10.0)) == StopStep::Wait);
    }
    SECTION("the same reading served twice") {
        StopVerifier verifier(at(0ms));
        CHECK(verifier.feed(at(100ms), status_at(100ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
        CHECK(verifier.feed(at(900ms), status_at(900ms, false), read_at(100ms, 1.0, 10.0)) == StopStep::Wait);
    }
}

TEST_CASE("StopVerifier - resends each second and fails at 3 s after the third stop", "[motion-guards]") {
    StopVerifier verifier(at(0ms));
    CHECK(verifier.feed(at(999ms), status_at(999ms, true), read_at(999ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(1000ms), status_at(1000ms, true), read_at(1000ms, 1.0, 10.0)) == StopStep::Resend);
    verifier.stop_resent(at(1000ms));
    CHECK(verifier.feed(at(2000ms), status_at(2000ms, true), read_at(2000ms, 1.0, 10.0)) == StopStep::Resend);
    verifier.stop_resent(at(2000ms));
    CHECK(verifier.stops_sent() == 3);
    CHECK(verifier.feed(at(2999ms), status_at(2999ms, true), read_at(2999ms, 1.0, 10.0)) == StopStep::Wait);
    CHECK(verifier.feed(at(3000ms), status_at(3000ms, true), read_at(3000ms, 1.0, 10.0)) == StopStep::Failed);
}

// ── Verified stop: the runner ─────────────────────────────────────────────────────────────────────────────────

TEST_CASE("run_verified_stop - moving for one round, then still: verified after two stops", "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    mount.moving = [](int stops) { return stops < 2; };
    mount.position = [](int stops, MotionTime now) {
        // Drifts 60 arcsec per poll until the second stop, then holds: 11 deg, and 10 arcsec off it from 1.75 s.
        const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
        if (stops < 2) {
            return MountPosition{1.0, 10.0 + t * 240 * kArcsecToDegrees};
        }
        return MountPosition{1.0, t >= 1.75 ? 11.0 + 10 * kArcsecToDegrees : 11.0};
    };
    std::optional<bool> verified;
    MotionTime ended{};
    const auto failure = run_in_slot(clock, [&](OperationContext& ctx) {
        verified = run_verified_stop(ctx, clock, mount.hooks());
        ended = clock.now();
    });
    CHECK_FALSE(failure.has_value());
    REQUIRE(verified.has_value());
    CHECK(*verified);
    CHECK(mount.stops == 2);
    // Second stop at 1 s; reads at 1.25 s and 1.75 s (500 ms apart) verify.
    CHECK(ended == at(1750ms));
}

TEST_CASE("run_verified_stop - a mount that never stops fails after exactly 3 stops in 3 s", "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    mount.moving = [](int) { return true; };
    MotionTime ended{};
    const auto failure = run_in_slot(clock, [&](OperationContext& ctx) {
        try {
            run_verified_stop(ctx, clock, mount.hooks());
        } catch (...) {
            ended = clock.now();
            throw;
        }
    });
    REQUIRE(failure.has_value());
    CHECK(*failure == "AbortSlew: the mount still reports motion after 3 stops");
    CHECK(std::string(kStopNotVerifiedText) == *failure);
    CHECK(mount.stops == 3);
    CHECK(ended == at(3000ms));
}

TEST_CASE("run_verified_stop - status stopped while the position drifts fails with the AbortSlew text",
          "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    mount.moving = [](int) { return false; };
    mount.position = [](int, MotionTime now) {
        const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
        return MountPosition{1.0, 10.0 + t * 200 * kArcsecToDegrees};  // 50 arcsec per poll
    };
    const auto failure =
        run_in_slot(clock, [&](OperationContext& ctx) { run_verified_stop(ctx, clock, mount.hooks()); });
    REQUIRE(failure.has_value());
    CHECK(*failure == kStopNotVerifiedText);
    CHECK(mount.stops == 3);
}

TEST_CASE("run_verified_stop - reads and stops that throw count as not verified", "[motion-guards]") {
    FakeTaskClock clock;
    std::atomic<int> stops{0};
    StopVerifyHooks hooks{
        [&] {
            ++stops;
            throw alpacacore::AlpacaException("link lost", AlpacaError::NotConnected);
        },
        []() -> std::optional<MotionStatusReading> { throw std::runtime_error("status read failed"); },
        []() -> std::optional<PositionReading> { throw std::runtime_error("position read failed"); },
    };
    const auto failure = run_in_slot(clock, [&](OperationContext& ctx) { run_verified_stop(ctx, clock, hooks); });
    REQUIRE(failure.has_value());
    CHECK(*failure == kStopNotVerifiedText);
    CHECK(stops == 3);
}

TEST_CASE("run_verified_stop - a body superseded before a resend sends nothing more", "[motion-guards]") {
    FakeTaskClock clock;
    OperationGeneration generation;
    AsyncOperation slot("motion-guards-test", generation, clock);
    ScriptedMount mount{clock};
    mount.moving = [](int) { return true; };
    // The fourth poll (1 s) is the one that would resend: a newer owner takes the axes just before it.
    mount.on_read = [&generation](int reads) {
        if (reads == 4) {
            generation.bump();
        }
    };
    std::optional<bool> verified;
    slot.start([&](OperationContext& ctx) { verified = run_verified_stop(ctx, clock, mount.hooks()); });
    for (int i = 0; i < 4; ++i) {
        REQUIRE(clock.wait_for_waiters(1, 2s));
        clock.advance(kStopVerifyPollInterval);
        REQUIRE(clock.wait_for_woken_settled(2s));
    }
    step_until_returned(clock, slot);
    CHECK_FALSE(slot.last_failure().has_value());
    REQUIRE(verified.has_value());
    CHECK_FALSE(*verified);
    CHECK(mount.stops == 1);
}

TEST_CASE("run_verified_stop - a body superseded before the first stop sends none", "[motion-guards]") {
    FakeTaskClock clock;
    OperationGeneration generation;
    AsyncOperation slot("motion-guards-test", generation, clock);
    ScriptedMount mount{clock};
    std::optional<bool> verified;
    slot.start([&](OperationContext& ctx) {
        generation.bump();  // superseded before the first send
        verified = run_verified_stop(ctx, clock, mount.hooks());
    });
    step_until_returned(clock, slot);
    REQUIRE(verified.has_value());
    CHECK_FALSE(*verified);
    CHECK(mount.stops == 0);
}

TEST_CASE("run_verified_stop - a body cancelled during its last reads returns without raising", "[motion-guards]") {
    FakeTaskClock clock;
    OperationGeneration generation;
    AsyncOperation slot("motion-guards-test", generation, clock);
    ScriptedMount mount{clock};
    mount.moving = [](int) { return true; };
    // The twelfth poll (3 s) is the one that fails the stop: an AbortSlew cancels the body while it reads.
    mount.on_read = [&slot](int reads) {
        if (reads == 12) {
            slot.cancel();
        }
    };
    std::optional<bool> verified;
    slot.start([&](OperationContext& ctx) { verified = run_verified_stop(ctx, clock, mount.hooks()); });
    step_until_returned(clock, slot);
    CHECK_FALSE(slot.last_failure().has_value());
    REQUIRE(verified.has_value());
    CHECK_FALSE(*verified);
    CHECK(mount.reads == 12);
    CHECK(mount.stops == 3);
}

// ── Slew deadline and MoveAxis cap ────────────────────────────────────────────────────────────────────────────

TEST_CASE("slew_time_budget - 2 x distance / rate + 10 s, capped at 300 s", "[motion-guards]") {
    CHECK(slew_time_budget(10.0, 2.0) == 20s);
    CHECK(slew_time_budget(0.0, 2.0) == 10s);
    CHECK(slew_time_budget(145.0, 1.0) == 300s);
    CHECK(slew_time_budget(144.5, 1.0) == 299s);
    CHECK(slew_time_budget(145.5, 1.0) == 300s);
    CHECK(slew_time_budget(180.0, 0.01) == 300s);
}

TEST_CASE("slew_time_budget - an unknown, zero, negative or non-finite rate gives the 300 s cap", "[motion-guards]") {
    CHECK(slew_time_budget(10.0, std::nullopt) == kMaxSlewDuration);
    CHECK(slew_time_budget(10.0, 0.0) == 300s);
    CHECK(slew_time_budget(10.0, -1.0) == 300s);
    CHECK(slew_time_budget(10.0, std::nan("")) == 300s);
    CHECK(slew_time_budget(10.0, std::numeric_limits<double>::infinity()) == 300s);
    CHECK(slew_time_budget(std::nan(""), 2.0) == 300s);
    CHECK(slew_time_budget(-10.0, 2.0) == 300s);
}

TEST_CASE("validate_move_axis_time_limit - 5 to 300 s, else InvalidValue", "[motion-guards]") {
    CHECK(kMaxMoveAxisDuration == 60s);
    CHECK(validate_move_axis_time_limit(5.0) == 5s);
    CHECK(validate_move_axis_time_limit(60.0) == 60s);
    CHECK(validate_move_axis_time_limit(300.0) == 300s);
    for (const double bad : {4.999, 300.001, 0.0, -60.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        CAPTURE(bad);
        alpacacore::test::require_alpaca_error([bad] { validate_move_axis_time_limit(bad); },
                                               AlpacaError::InvalidValue);
    }
}

namespace {

// A timed motion polled every 1 ms in a slot: the body runs enforce_motion_deadline() each poll. Advances virtual
// time to 1 ms before the deadline (nothing sent), then to the deadline, then steps the stop through. Returns the
// slot's kept failure.
std::optional<std::string> run_deadline(FakeTaskClock& clock, ScriptedMount& mount, std::chrono::nanoseconds budget,
                                        const std::string& expired_text) {
    HoldWindow deadline;
    deadline.arm(clock.now(), budget);
    const auto hooks = mount.hooks();
    OperationGeneration generation;
    AsyncOperation slot("motion-guards-test", generation, clock);
    slot.start([&](OperationContext& ctx) {
        while (enforce_motion_deadline(ctx, clock, deadline, hooks, expired_text)) {
            if (!ctx.wait_for(1ms)) {
                return;
            }
        }
    });
    REQUIRE(clock.wait_for_waiters(1, 2s));
    clock.advance(budget - 1ms);
    REQUIRE(clock.wait_for_woken_settled(2s));
    REQUIRE(clock.wait_for_waiters(1, 2s));
    CHECK(mount.stops == 0);
    clock.advance(1ms);
    REQUIRE(clock.wait_for_woken_settled(2s));
    CHECK(mount.stops == 1);
    step_until_returned(clock, slot);
    return slot.last_failure();
}

}  // namespace

TEST_CASE("enforce_motion_deadline - a slew past its deadline is stopped, verified and raises", "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    const auto failure =
        run_deadline(clock, mount, slew_time_budget(10.0, 2.0), "SlewToCoordinatesAsync: the slew ran past 20 s");
    REQUIRE(failure.has_value());
    CHECK(*failure == "SlewToCoordinatesAsync: the slew ran past 20 s");
    CHECK(mount.stops == 1);
}

TEST_CASE("enforce_motion_deadline - a slew past its deadline whose stop does not verify raises the AbortSlew text",
          "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    mount.moving = [](int) { return true; };
    const auto failure = run_deadline(clock, mount, kMaxSlewDuration, "the slew ran past 300 s");
    REQUIRE(failure.has_value());
    CHECK(*failure == kStopNotVerifiedText);
    CHECK(mount.stops == 3);
}

TEST_CASE("enforce_motion_deadline - a MoveAxis past its 60 s cap is stopped, verified and raises", "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    const auto failure = run_deadline(clock, mount, kMaxMoveAxisDuration, "MoveAxis ran past its 60 s limit");
    REQUIRE(failure.has_value());
    CHECK(*failure == "MoveAxis ran past its 60 s limit");
    CHECK(mount.stops == 1);
}

TEST_CASE("enforce_motion_deadline - a MoveAxis past its cap whose stop does not verify raises the AbortSlew text",
          "[motion-guards]") {
    FakeTaskClock clock;
    ScriptedMount mount{clock};
    mount.moving = [](int) { return true; };
    const auto failure = run_deadline(clock, mount, validate_move_axis_time_limit(5.0), "MoveAxis ran past 5 s");
    REQUIRE(failure.has_value());
    CHECK(*failure == kStopNotVerifiedText);
    CHECK(mount.stops == 3);
}
