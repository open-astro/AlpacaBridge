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

// StateSnapshot (decision record docs/decisions/0010-snapshot-served-state.md)
// under the virtual FakeTaskClock: no real sleeps.

#include <alpacacore/util/state_snapshot.h>

#include <chrono>

#include "catch2_compat.h"
#include "fake_task_clock.h"

using alpacacore::test::FakeTaskClock;
using alpacacore::util::StateSnapshot;
using namespace std::chrono_literals;

namespace {
struct Pointing {
    double ra = 0.0;
    double dec = 0.0;
};
}  // namespace

TEST_CASE("StateSnapshot - publish, read, stale", "[state_snapshot][unit]") {
    FakeTaskClock clock;
    StateSnapshot<Pointing> snap(clock, 2s);

    REQUIRE_FALSE(snap.read().has_value());

    const auto token = snap.begin_poll();
    REQUIRE(snap.publish(token, Pointing{1.5, 20.0}));

    auto fresh = snap.read();
    REQUIRE(fresh.has_value());
    CHECK(fresh->value.ra == 1.5);
    CHECK(fresh->measured_at == clock.now());
    CHECK_FALSE(fresh->stale);

    clock.advance(2s);
    CHECK_FALSE(snap.read()->stale);  // exactly at the bound is not yet stale
    clock.advance(1ms);
    auto old = snap.read();
    CHECK(old->stale);
    CHECK(old->value.ra == 1.5);  // stale values are still reported, flagged

    snap.reset();
    CHECK_FALSE(snap.read().has_value());
}

TEST_CASE("StateSnapshot - write-through is visible before the next poll", "[state_snapshot][unit]") {
    FakeTaskClock clock;
    StateSnapshot<Pointing> snap(clock, 2s);
    REQUIRE(snap.publish(snap.begin_poll(), Pointing{1.0, 2.0}));

    clock.advance(500ms);
    snap.write([](Pointing& p) { p.ra = 9.0; });

    auto r = snap.read();
    CHECK(r->value.ra == 9.0);
    CHECK(r->value.dec == 2.0);  // untouched fields keep the polled value
    CHECK(r->measured_at == clock.now());
}

TEST_CASE("StateSnapshot - a late publish does not overwrite a newer write", "[state_snapshot][unit]") {
    FakeTaskClock clock;
    StateSnapshot<Pointing> snap(clock, 2s);
    REQUIRE(snap.publish(snap.begin_poll(), Pointing{1.0, 2.0}));

    const auto token = snap.begin_poll();  // the poll samples here ...
    clock.advance(100ms);
    snap.write([](Pointing& p) { p.ra = 9.0; });  // ... a client write lands ...
    CHECK_FALSE(snap.publish(token, Pointing{1.0, 2.0}));  // ... the poll's older frame arrives late

    CHECK(snap.read()->value.ra == 9.0);

    // The next poll, started after the write, publishes normally.
    CHECK(snap.publish(snap.begin_poll(), Pointing{9.0, 2.5}));
    CHECK(snap.read()->value.dec == 2.5);
}
