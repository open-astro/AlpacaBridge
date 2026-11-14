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

// util::StreamLinkHealth::fault_stale() (decision 0009 point 2(b), issue #1988): a silence fault that has stood
// for util::kLinkStalenessBound is a lost link. Pure and single-threaded: time points are built by hand.

#include <alpacacore/util/link_health.h>

#include <chrono>

#include "catch2_compat.h"

using alpacacore::util::kLinkStalenessBound;
using alpacacore::util::StreamLinkHealth;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

constexpr milliseconds kLimit{10000};  // the Wanderer streamers' silence limit

// Latches a fault on @p link at start + 10 s + 1 ms and returns that instant.
StreamLinkHealth::clock::time_point latch(StreamLinkHealth& link, StreamLinkHealth::clock::time_point start) {
    link.reset(start);
    const auto at = start + kLimit + milliseconds(1);
    REQUIRE(link.check_silence(at, kLimit).has_value());
    return at;
}

}  // namespace

// Falsified by: AlpacaCore/include/alpacacore/util/link_health.h StreamLinkHealth::check_silence stamping
// last_frame_ instead of now as fault_since_.
TEST_CASE("Stream link health - a fault is stale once it has stood for the staleness bound",
          "[util][link_health][unit]") {
    StreamLinkHealth link;
    const auto start = StreamLinkHealth::clock::time_point{} + seconds(1000);
    link.reset(start);
    // Silent but not yet latched: never stale, however long the silence.
    CHECK_FALSE(link.fault_stale(start + kLimit));
    const auto latched_at = latch(link, start);
    CHECK_FALSE(link.fault_stale(latched_at));
    CHECK_FALSE(link.fault_stale(latched_at + kLinkStalenessBound - milliseconds(1)));
    CHECK(link.fault_stale(latched_at + kLinkStalenessBound));
    CHECK(link.fault_stale(latched_at + kLinkStalenessBound + seconds(60)));
    // A driver may pass a longer bound; the stamp is the same.
    CHECK_FALSE(link.fault_stale(latched_at + kLinkStalenessBound, seconds(45)));
    CHECK(link.fault_stale(latched_at + seconds(45), seconds(45)));
}

// Falsified by: AlpacaCore/include/alpacacore/util/link_health.h StreamLinkHealth::on_frame not clearing
// fault_, so the fault stands and goes stale through the frame.
TEST_CASE("Stream link health - a frame clears the fault and its stamp", "[util][link_health][unit]") {
    StreamLinkHealth link;
    const auto start = StreamLinkHealth::clock::time_point{} + seconds(1000);
    const auto first = latch(link, start);
    CHECK(link.on_frame(first + seconds(20)));
    CHECK_FALSE(link.faulted());
    CHECK_FALSE(link.fault_stale(first + kLinkStalenessBound));

    // A second fault is timed from its own latch, not from the first one.
    const auto second = first + seconds(20) + kLimit + milliseconds(1);
    REQUIRE(link.check_silence(second, kLimit).has_value());
    CHECK_FALSE(link.fault_stale(first + kLinkStalenessBound + seconds(5)));
    CHECK(link.fault_stale(second + kLinkStalenessBound));
}

// Falsified by: AlpacaCore/include/alpacacore/util/link_health.h StreamLinkHealth::reset not clearing fault_,
// so the old session's fault stands and goes stale after a reconnect.
TEST_CASE("Stream link health - a reset clears the fault and its stamp", "[util][link_health][unit]") {
    StreamLinkHealth link;
    const auto start = StreamLinkHealth::clock::time_point{} + seconds(1000);
    const auto first = latch(link, start);
    const auto reconnect = first + seconds(5);
    link.reset(reconnect);
    CHECK_FALSE(link.faulted());
    CHECK_FALSE(link.fault_stale(first + kLinkStalenessBound));

    // The next fault after the reset starts its own bound.
    const auto second = reconnect + kLimit + milliseconds(1);
    REQUIRE(link.check_silence(second, kLimit).has_value());
    CHECK_FALSE(link.fault_stale(first + kLinkStalenessBound + seconds(5)));
    CHECK(link.fault_stale(second + kLinkStalenessBound));
}
