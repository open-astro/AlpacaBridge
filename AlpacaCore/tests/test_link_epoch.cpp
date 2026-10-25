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

// LinkEpoch (issue #1500, decision 0009): one atomic word, epoch << 1 | open bit. Every Connect and
// Disconnect starts a new epoch; a loss applies only to the epoch the failing call captured, so a stale
// failure that lands after a relink cannot drop the new link. Single-threaded and deterministic: the
// interleaving a driver test cannot force is written out step by step.

#include <alpacacore/util/link_epoch.h>

#include "catch2_compat.h"

using alpacacore::util::LinkEpoch;

// Falsified by: mark_lost leaving the open bit set, applying a second loss of the same epoch, or reporting a
// loss for a word that was already closed.
TEST_CASE("Link epoch - A loss of the current epoch drops the link once", "[util][link]") {
    LinkEpoch link;
    CHECK_FALSE(link.is_open());
    CHECK_FALSE(link.mark_lost(link.load()));  // a closed word has no link to lose
    link.begin_transition();
    CHECK_FALSE(link.mark_lost(link.load()));
    const auto captured = link.publish_open();
    REQUIRE(link.is_open());

    CHECK(link.mark_lost(captured));
    CHECK_FALSE(link.is_open());
    CHECK_FALSE(link.mark_lost(captured));  // the loss is applied, and logged, once
    CHECK_FALSE(link.is_open());
}

// Falsified by: mark_lost storing the closed word without comparing the captured epoch (no CAS).
TEST_CASE("Link epoch - A loss captured before a relink is a no-op", "[util][link]") {
    LinkEpoch link;
    link.begin_transition();
    const auto before_removal = link.publish_open();
    CHECK(link.mark_lost(before_removal));  // the removal, seen by one call

    link.begin_transition();  // the client's Connect relinks the replugged device
    const auto relinked = link.publish_open();
    REQUIRE(link.is_open());
    CHECK(LinkEpoch::epoch_of(relinked) > LinkEpoch::epoch_of(before_removal));

    // A second call that took the old id before the removal fails only now.
    CHECK_FALSE(link.mark_lost(before_removal));
    CHECK(link.is_open());
    CHECK(link.load() == relinked);

    // A word captured while closed (a call racing the Connect) never drops the link either.
    link.begin_transition();
    const auto closed = link.load();
    CHECK_FALSE(LinkEpoch::is_open(closed));
    const auto reopened = link.publish_open();
    CHECK_FALSE(link.mark_lost(closed));
    CHECK(link.load() == reopened);
}
