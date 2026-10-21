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

#include <alpacacore/util/utc_offset.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "fake_task_clock.h"

using alpacacore::test::FakeTaskClock;
using alpacacore::util::UtcOffset;
using namespace std::chrono_literals;

namespace {
const UtcOffset::utc kWritten = std::chrono::system_clock::time_point{} + std::chrono::seconds(1'800'000'000) + 250ms;
}

TEST_CASE("UtcOffset - a fresh object holds no value", "[util][utc-offset]") {
    FakeTaskClock clock;
    UtcOffset offset;
    CHECK_FALSE(offset.valid());
    CHECK_FALSE(offset.current(clock.now()).has_value());
}

TEST_CASE("UtcOffset - current is the written value plus the clock time elapsed since", "[util][utc-offset]") {
    FakeTaskClock clock;
    UtcOffset offset;
    offset.set(kWritten, clock.now());
    CHECK(offset.valid());
    CHECK(*offset.current(clock.now()) == kWritten);
    clock.advance(90s);
    CHECK(*offset.current(clock.now()) == kWritten + 90s);
    clock.advance(500ms);
    CHECK(*offset.current(clock.now()) == kWritten + 90s + 500ms);
}

TEST_CASE("UtcOffset - the later set wins and forgets the earlier anchor", "[util][utc-offset]") {
    FakeTaskClock clock;
    UtcOffset offset;
    offset.set(kWritten, clock.now());
    clock.advance(100s);
    const auto second = kWritten + 5h;
    offset.set(second, clock.now());
    CHECK(*offset.current(clock.now()) == second);
    clock.advance(10s);
    CHECK(*offset.current(clock.now()) == second + 10s);
}

TEST_CASE("UtcOffset - clear drops the value even after the clock moves on", "[util][utc-offset]") {
    FakeTaskClock clock;
    UtcOffset offset;
    offset.set(kWritten, clock.now());
    offset.clear();
    CHECK_FALSE(offset.valid());
    CHECK_FALSE(offset.current(clock.now()).has_value());
    clock.advance(30s);
    CHECK_FALSE(offset.current(clock.now()).has_value());
    offset.set(kWritten, clock.now());
    CHECK(offset.valid());
}

TEST_CASE("UtcOffset - a now before the anchor subtracts the difference", "[util][utc-offset]") {
    FakeTaskClock clock;
    UtcOffset offset;
    offset.set(kWritten, clock.now() + 10s);
    CHECK(*offset.current(clock.now()) == kWritten - 10s);
}
