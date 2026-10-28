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

#include <alpacacore/util/ttl_status_cache.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "catch2_compat.h"
#include "getter_stall_probe.h"

namespace {

using alpacacore::util::TtlStatusCache;
using namespace std::chrono_literals;

void fail_refills(TtlStatusCache<int>& cache, int n) {
    for (int i = 0; i < n; ++i) {
        try {
            (void)cache.get([]() -> int { throw std::runtime_error("link down"); });
        } catch (const std::exception&) {
        }
    }
}

}  // namespace

// Falsified by: ttl_status_cache.h fault() taking mutex_ (the call waits for the blocked refill).
TEST_CASE("TtlStatusCache - fault() does not wait for a refill in flight", "[util][ttl-cache][getter-stall]") {
    using namespace alpacacore::test;
    TtlStatusCache<int> cache("Dev", 1s);
    std::atomic<bool> in_fetch{false};
    std::promise<void> release;
    auto latch = release.get_future().share();

    std::thread refill([&] {
        (void)cache.get([&] {
            in_fetch = true;
            latch.wait();
            return 1;
        });
    });
    REQUIRE(wait_for_flag(in_fetch));

    // On its own thread so a fault() that waits fails the check instead of hanging the test.
    auto probe = std::async(std::launch::async, [&] { return alpacacore::test::timed([&] { (void)cache.fault(); }); });
    const bool answered = probe.wait_for(500ms) == std::future_status::ready;
    CHECK(answered);

    release.set_value();
    if (answered) CHECK(probe.get() < kGetterBudget);
    refill.join();
}

// Falsified by: ttl_status_cache.h get() not copying health_.fault() into fault_ after note_failure() (fault() stays
// empty).
TEST_CASE("TtlStatusCache - fault() returns the latched text after the threshold", "[util][ttl-cache]") {
    TtlStatusCache<int> cache("Dev", 1s, 3);
    fail_refills(cache, 2);
    CHECK(cache.fault().empty());
    fail_refills(cache, 1);
    CHECK(cache.fault().find("link down") != std::string::npos);
}

// Falsified by: ttl_status_cache.h reset() or the on_reply path not refreshing fault_ (the stale text survives).
TEST_CASE("TtlStatusCache - fault() is empty after reset() and after a good refill", "[util][ttl-cache]") {
    TtlStatusCache<int> cache("Dev", 1s, 3);
    fail_refills(cache, 3);
    REQUIRE_FALSE(cache.fault().empty());
    cache.reset();
    CHECK(cache.fault().empty());

    fail_refills(cache, 3);
    REQUIRE_FALSE(cache.fault().empty());
    CHECK(cache.get([] { return 5; }) == 5);
    CHECK(cache.fault().empty());
}
