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

// Bisque Connected takes no lock the connect holds and reads false for as long
// as Connecting is true (decision 0016, issue #910). Every TheSkyX reply is
// held, so the handshake stays on the wire for a known time.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "fake_bisque_mount.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

TEST_CASE("Bisque telescope - Connected answers at once and reads false while a connect is on the wire",
          "[bisque][telescope][getter-locks]") {
    using std::chrono::milliseconds;
    using std::chrono::steady_clock;

    alpacacore::test::FakeBisqueMount mount;
    alpacacore::test::FakeTaskClock clock;  // no status poll tick lands during the case
    alpacacore::test::FakeMountServer server{mount.responder()};
    REQUIRE(server.ok());
    alpacacore::vendor::bisque::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 3000;
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, info, clock);

    mount.latency_ms = 300;
    const auto start = steady_clock::now();
    driver->connect();
    for (int i = 0; i < 200 && mount.transactions.load() == 0; ++i) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(mount.transactions.load() > 0);

    int samples = 0;
    while (driver->get_connecting() && samples < 40) {
        const auto before = steady_clock::now();
        CHECK_FALSE(driver->get_connected());
        CHECK(steady_clock::now() - before < milliseconds(100));
        ++samples;
        std::this_thread::sleep_for(milliseconds(10));
    }
    CHECK(samples > 0);
    CHECK(steady_clock::now() - start > milliseconds(300));  // the window overlapped the connect

    for (int i = 0; i < 400 && driver->get_connecting(); ++i) {
        std::this_thread::sleep_for(milliseconds(25));
    }
    REQUIRE_FALSE(driver->get_connecting());
    mount.latency_ms = 0;
    CHECK(driver->get_connected());
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}
