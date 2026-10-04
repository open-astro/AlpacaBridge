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

// open-astro#768, iOptron: AbortSlew must fence the async slew dispatch thread.
// Once it returns, no :MS1#/:MS2# may reach the mount and the stop has been
// sent, even when the cached status says the mount is not slewing.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_ioptron_mount.h"

namespace {

alpacacore::vendor::ioptron::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    // iOptron's read_response() sleeps 10 ms per character; see
    // test_ioptron_async_slew_failure.cpp.
    conn.response_timeout_ms = 2000;
    return conn;
}

}  // namespace

TEST_CASE("iOptron AbortSlew - no GOTO reaches the mount after it returns", "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const int iterations = 20;
    int goto_after_abort = 0;
    int missing_stop = 0;
    for (int i = 0; i < iterations; ++i) {
        const int q_before = mount.count(":Q#");
        driver->slew_to_coordinates_async(5.0 + 0.1 * i, 20.0);
        driver->abort_slew();
        const int ms_at_return = mount.count(":MS1#") + mount.count(":MS2#");
        if (mount.count(":Q#") <= q_before) {
            ++missing_stop;
        }
        // Let any dispatch that escaped the fence reach the mount.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mount.count(":MS1#") + mount.count(":MS2#") > ms_at_return) {
            ++goto_after_abort;
        }
    }
    CHECK(goto_after_abort == 0);
    CHECK(missing_stop == 0);
    driver->set_connected(false);
}

TEST_CASE("iOptron AbortSlew - sends :Q# when cached status says not slewing", "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const int q_before = mount.count(":Q#");
    REQUIRE_FALSE(driver->get_slewing());
    driver->abort_slew();
    CHECK(mount.count(":Q#") > q_before);
    driver->set_connected(false);
}

#endif
