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

#pragma once

// Shared cases for connect-time auto-detect (issue #659). Every auto-detect
// factory used to run its scan at construction, i.e. at server start-up for a
// persisted device, so a mount still joining Wi-Fi or a USB adapter powered
// after the SBC left the entry as "(failed to load)" until a restart. Each
// vendor test file builds its driver through the `create_*_deferred` seam
// with a resolver the case controls and calls these three checks:
//
//   1. refused:   the resolver throws -> construction succeeds, the connect
//                 refusal carries the scan's message (sync and async), the
//                 driver stays disconnected.
//   2. reused:    the resolver finds the fake -> connected; a reconnect reuses
//                 the resolved endpoint without a second scan.
//   3. re-resolved: the fake behind the resolved endpoint goes away and a new
//                 one appears -> the retry of the old endpoint fails, the
//                 resolver runs again, the driver connects to the new fake.
//
// `Spawn` is a callable that (re)creates the vendor's fake and returns the
// endpoint to reach it. It owns the fake; replacing it must construct the
// new fake BEFORE destroying the old one so the two never share a port or
// pty path.

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected

namespace alpacacore::test {

// A stale serial endpoint can cost a driver its whole handshake retry ladder
// (Gemini: ~9 s) before the re-scan runs, so the connected settles get more
// than settle_connected's 10 s default.
inline constexpr std::chrono::seconds kSlowBudget{40};

template <typename Info>
using DeferredFactory = std::function<std::unique_ptr<AlpacaDriver>(util::ConnectionResolver<Info>)>;

inline bool wait_connect_task_done(AlpacaDriver& driver, std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!driver.get_connecting()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !driver.get_connecting();
}

/// Case 1: a scan that finds nothing refuses the connect with its own message
/// and never breaks construction.
template <typename Info>
void check_deferred_connect_refused(const DeferredFactory<Info>& make, const std::string& refusal) {
    int calls = 0;
    auto driver = make([&calls, refusal]() -> Info {
        ++calls;
        throw AlpacaException(refusal);
    });
    REQUIRE(driver != nullptr);
    CHECK(calls == 0);  // construction never scans
    CHECK_FALSE(driver->get_connected());

    // Sync path: the refusal is the exception the client sees.
    try {
        driver->set_connected(true);
        FAIL("set_connected(true) must throw when the scan finds nothing");
    } catch (const AlpacaException& ex) {
        CHECK(std::string(ex.what()).find(refusal) != std::string::npos);
    }
    CHECK(calls == 1);
    CHECK_FALSE(driver->get_connected());

    // Async path (Platform 7 Connect): the same text lands in LastConnectError.
    driver->connect();
    REQUIRE(wait_connect_task_done(*driver));
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_last_connect_error().find(refusal) != std::string::npos);
    CHECK(calls == 2);
}

/// Case 2: a scan that finds the fake connects, and the next connect reuses
/// that endpoint instead of scanning again.
template <typename Info>
void check_deferred_connect_reuses_endpoint(const DeferredFactory<Info>& make, const std::function<Info()>& spawn) {
    int calls = 0;
    Info current = spawn();
    auto driver = make([&calls, &current]() -> Info {
        ++calls;
        return current;
    });
    REQUIRE(driver != nullptr);
    CHECK(calls == 0);

    REQUIRE(settle_connected(*driver, true, kSlowBudget));
    CHECK(calls == 1);
    REQUIRE(settle_connected(*driver, false));
    // One direct connect, not settle_connected(): its retry loop swallows a
    // transient failure and re-runs the resolver, which would turn a healthy
    // reuse into a spurious second scan. A failure here is a real failure.
    REQUIRE_NOTHROW(driver->set_connected(true));
    REQUIRE(driver->get_connected());
    CHECK(calls == 1);  // the resolved endpoint answered: no second scan
    REQUIRE(settle_connected(*driver, false));
}

/// Case 3: the resolved endpoint dies and a new device appears; the retry of
/// the old endpoint fails and the scan runs again.
template <typename Info>
void check_deferred_connect_re_resolves(const DeferredFactory<Info>& make, const std::function<Info()>& spawn) {
    int calls = 0;
    Info current = spawn();
    auto driver = make([&calls, &current]() -> Info {
        ++calls;
        return current;
    });
    REQUIRE(settle_connected(*driver, true, kSlowBudget));
    CHECK(calls == 1);
    REQUIRE(settle_connected(*driver, false));

    current = spawn();  // the old fake is gone, a new one answers elsewhere
    REQUIRE(settle_connected(*driver, true, kSlowBudget));
    // At least one fresh scan; settle_connected may retry a transient failure
    // of the new endpoint, which scans again.
    CHECK(calls >= 2);
    REQUIRE(settle_connected(*driver, false));
}

}  // namespace alpacacore::test
