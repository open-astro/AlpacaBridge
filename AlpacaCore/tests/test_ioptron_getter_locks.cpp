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

// iOptron Connected never waits on a connect (decision 0016, issue #910). The
// connect holds the driver's state lock across the handshake, so a getter that
// takes that lock waits for the whole connect. The mount fake holds its next
// reply, which keeps the handshake on the wire for a known time.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "fake_ioptron_mount.h"
#include "fake_task_clock.h"

TEST_CASE("iOptron telescope - Connected answers at once and reads false while a connect is on the wire",
          "[ioptron][telescope][getter-locks]") {
    using std::chrono::milliseconds;
    using std::chrono::steady_clock;
    namespace ioptron = alpacacore::vendor::ioptron;

    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;  // no clock-sync tick lands during the case
    ioptron::ConnectionInfo conn;
    conn.type = ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = mount.port();
    auto driver = ioptron::create_ioptron_telescope(0, conn, clock);

    constexpr auto kHold = milliseconds(1500);
    mount.hold_next_reply(kHold);
    const auto start = steady_clock::now();
    driver->connect();
    REQUIRE(mount.wait_until_holding(std::chrono::seconds(5)));

    int samples = 0;
    while (steady_clock::now() - start < milliseconds(1000)) {
        const auto before = steady_clock::now();
        CHECK(driver->get_connecting());
        CHECK_FALSE(driver->get_connected());
        CHECK(steady_clock::now() - before < milliseconds(100));
        ++samples;
        std::this_thread::sleep_for(milliseconds(20));
    }
    CHECK(samples > 0);

    for (int i = 0; i < 200 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(milliseconds(50));
    }
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_connected());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

#endif  // _WIN32
