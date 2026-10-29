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

// util::HoldWindow (decision 0022). Pure value type: the cases pass `now`
// themselves, so there is no clock and nothing to wait on.

#include <alpacacore/util/hold_window.h>

#include <chrono>

#include "catch2_compat.h"
#include "fake_task_clock.h"

using alpacacore::test::FakeTaskClock;
using alpacacore::util::HoldWindow;
using namespace std::chrono_literals;

TEST_CASE("HoldWindow - a fresh window is inactive", "[util][hold-window]") {
    FakeTaskClock clock;
    HoldWindow window;
    CHECK_FALSE(window.active(clock.now()));
}

TEST_CASE("HoldWindow - active strictly before the deadline", "[util][hold-window]") {
    FakeTaskClock clock;
    HoldWindow window;
    const auto t0 = clock.now();
    window.arm(t0, 8s);
    CHECK(window.active(t0));
    CHECK(window.active(t0 + 7999ms));
    CHECK_FALSE(window.active(t0 + 8s));
    CHECK_FALSE(window.active(t0 + 9s));
}

TEST_CASE("HoldWindow - clear closes the window at once", "[util][hold-window]") {
    FakeTaskClock clock;
    HoldWindow window;
    const auto t0 = clock.now();
    window.arm(t0, 8s);
    window.clear();
    CHECK_FALSE(window.active(t0));
    CHECK_FALSE(window.active(t0 + 1s));
}

TEST_CASE("HoldWindow - a later arm replaces the deadline, longer or shorter", "[util][hold-window]") {
    FakeTaskClock clock;
    HoldWindow window;
    const auto t0 = clock.now();
    window.arm(t0, 2s);
    window.arm(t0, 5s);  // longer extends
    CHECK(window.active(t0 + 4999ms));
    CHECK_FALSE(window.active(t0 + 5s));
    window.arm(t0, 2s);  // shorter shortens (decision 0022: unconditional)
    CHECK(window.active(t0 + 1999ms));
    CHECK_FALSE(window.active(t0 + 2s));
}

TEST_CASE("HoldWindow - a zero hold is inactive at once", "[util][hold-window]") {
    FakeTaskClock clock;
    HoldWindow window;
    const auto t0 = clock.now();
    window.arm(t0, 0s);
    CHECK_FALSE(window.active(t0));
}
