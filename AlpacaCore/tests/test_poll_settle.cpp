// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

// Deterministic tests for the poll-until-settled decision logic (issue #105).
// Each sequence runs in microseconds — no sleeps, no hardware, no clock. The
// AFW parameters (3 stable reads, 60-poll budget) mirror wait_for_home.

#include <alpacacore/util/poll_settle.h>

#include <initializer_list>
#include <stdexcept>

#include "catch2_compat.h"

using alpacacore::util::ConsecutiveSettle;
using State = alpacacore::util::ConsecutiveSettle::State;

namespace {

// Feed a wheel-position sequence (-1 = moving) and return the final state.
State run(ConsecutiveSettle& settle, std::initializer_list<int> positions) {
    State state = State::Pending;
    for (int pos : positions) {
        state = settle.feed(pos >= 0);
        if (state != State::Pending) {
            break;
        }
    }
    return state;
}

}  // namespace

TEST_CASE("ConsecutiveSettle - normal home settles after the stable run", "[util][settle][unit]") {
    ConsecutiveSettle settle(3, 60);
    CHECK(run(settle, {-1, -1, -1, 4, 4, 4}) == State::Settled);
}

TEST_CASE("ConsecutiveSettle - deceleration bounce must not settle early (PR #99 r18)", "[util][settle][unit]") {
    // A real-slot read mid-move, then the firmware reports moving again: the
    // bounce read must reset the run, not count toward settling.
    ConsecutiveSettle bounced(3, 60);
    CHECK(bounced.feed(true) == State::Pending);   // bounce read
    CHECK(bounced.feed(false) == State::Pending);  // still moving -> run resets
    CHECK(bounced.feed(true) == State::Pending);   // stable 1
    CHECK(bounced.feed(true) == State::Pending);   // stable 2
    CHECK(bounced.feed(true) == State::Settled);   // stable 3 -> settled
}

TEST_CASE("ConsecutiveSettle - two bounces still require a full fresh run", "[util][settle][unit]") {
    ConsecutiveSettle settle(3, 60);
    CHECK(run(settle, {2, -1, 2, 2, -1, 0, 0, 0}) == State::Settled);
}

TEST_CASE("ConsecutiveSettle - fast homer that never reports moving", "[util][settle][unit]") {
    // A wheel already at its slot reports real positions from the first read.
    ConsecutiveSettle settle(3, 60);
    CHECK(settle.feed(true) == State::Pending);
    CHECK(settle.feed(true) == State::Pending);
    CHECK(settle.feed(true) == State::Settled);
}

TEST_CASE("ConsecutiveSettle - never settles -> TimedOut exactly at the poll budget", "[util][settle][unit]") {
    ConsecutiveSettle settle(3, 60);
    for (int i = 0; i < 59; ++i) {
        REQUIRE(settle.feed(false) == State::Pending);
    }
    CHECK(settle.feed(false) == State::TimedOut);
}

TEST_CASE("ConsecutiveSettle - settling on the final poll wins over the budget", "[util][settle][unit]") {
    ConsecutiveSettle settle(3, 60);
    for (int i = 0; i < 57; ++i) {
        REQUIRE(settle.feed(false) == State::Pending);
    }
    REQUIRE(settle.feed(true) == State::Pending);  // poll 58
    REQUIRE(settle.feed(true) == State::Pending);  // poll 59
    CHECK(settle.feed(true) == State::Settled);    // poll 60: settle beats timeout
}

TEST_CASE("ConsecutiveSettle - an almost-run that ends moving times out", "[util][settle][unit]") {
    // 58 moving reads, two real-slot reads, then moving again on the final
    // poll: the run never reaches 3, so the budget expires.
    ConsecutiveSettle settle(3, 61);
    for (int i = 0; i < 58; ++i) {
        REQUIRE(settle.feed(false) == State::Pending);
    }
    REQUIRE(settle.feed(true) == State::Pending);
    REQUIRE(settle.feed(true) == State::Pending);
    CHECK(settle.feed(false) == State::TimedOut);
}

TEST_CASE("ConsecutiveSettle - terminal states are sticky", "[util][settle][unit]") {
    ConsecutiveSettle settle(1, 2);
    REQUIRE(settle.feed(true) == State::Settled);
    CHECK(settle.feed(false) == State::Settled);  // further feeds don't regress

    ConsecutiveSettle timed(1, 1);
    REQUIRE(timed.feed(false) == State::TimedOut);
    CHECK(timed.feed(true) == State::TimedOut);
}

TEST_CASE("ConsecutiveSettle - zero/negative parameters are rejected loudly", "[util][settle][unit]") {
    CHECK_THROWS_AS(ConsecutiveSettle(0, 60), std::invalid_argument);
    CHECK_THROWS_AS(ConsecutiveSettle(3, 0), std::invalid_argument);
    CHECK_THROWS_AS(ConsecutiveSettle(-1, -1), std::invalid_argument);
    // Settled would be statically unreachable: run longer than the budget.
    CHECK_THROWS_AS(ConsecutiveSettle(5, 3), std::invalid_argument);
}
