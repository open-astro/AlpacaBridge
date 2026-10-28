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

#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "fake_zwo_caa_sdk.h"
#include "getter_stall_probe.h"
#include "rotator_sync_offset_test_dir.h"

// get_reverse() reads the status cache's fault(): it must not wait behind a refill queued on io_mutex_ (decision 0016).
// Falsified by: ttl_status_cache.h fault() taking mutex_ (get_reverse() waits for the queued refill, which waits for
// the move or read in flight).
TEST_CASE("ZWO CAA Rotator Driver - get_reverse answers while a refill is queued or in flight",
          "[zwo][rotator][unit][fake-sdk][getter-stall]") {
    using namespace alpacacore::test;
    TempSyncOffsetDir dir;
    FakeZWOCAASDK sdk;
    auto driver = alpacacore::vendor::zwo::create_zwo_caa_rotator(0, sdk.id, sdk);
    driver->set_connected(true);

    std::thread holder;
    std::thread reader;
    SECTION("a move holds the I/O lock and a refill is queued behind it") {
        sdk.move_delay = kHoldOpen;
        holder = std::thread([&] {
            try {
                driver->move_absolute(90.0);
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.move_entered));
        reader = std::thread([&] {
            try {
                (void)driver->get_is_moving();
            } catch (const std::exception&) {
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the refill take the cache lock and queue
    }
    SECTION("a status read is in flight") {
        sdk.read_delay = kHoldOpen;
        reader = std::thread([&] {
            try {
                (void)driver->get_is_moving();
            } catch (const std::exception&) {
            }
        });
        REQUIRE(wait_for_flag(sdk.read_entered));
    }

    CHECK(timed([&] { (void)driver->get_reverse(); }) < kGetterBudget);

    if (holder.joinable()) holder.join();
    reader.join();
    driver->set_connected(false);
}
