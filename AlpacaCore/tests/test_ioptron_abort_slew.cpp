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
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

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
    auto target_ra = [](int i) { return 5.0 + 0.1 * i; };
    std::vector<std::size_t> log_at_return;
    int missing_stop = 0;
    for (int i = 0; i < iterations; ++i) {
        const int q_before = mount.count(":Q#");
        driver->slew_to_coordinates_async(target_ra(i), 20.0);
        driver->abort_slew();
        log_at_return.push_back(mount.commands().size());
        if (mount.count(":Q#") <= q_before) {
            ++missing_stop;
        }
        // An escaped dispatch is already waiting on the driver mutex when
        // AbortSlew returns, so its :SRA lands within milliseconds; let it in
        // before the next initiator's reap would cancel it and hide the bug.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (mount.commands().size() == log_at_return.back() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // A new initiator joins the previous dispatch before it starts its own, so
    // once this pair returns every dispatch above has finished. An escaped GOTO
    // lands ~0.6 s after AbortSlew returns on this fake, too late for a sleep.
    driver->slew_to_coordinates_async(4.0, 20.0);
    driver->abort_slew();
    const auto log = mount.commands();

    // Tie each :MS1#/:MS2# to its iteration through the :SRA before it (RA in
    // 0.01 arcsec units), and count those sent after that iteration's AbortSlew.
    int goto_after_abort = 0;
    int owner = -1;
    for (std::size_t k = 0; k < log.size(); ++k) {
        const std::string& cmd = log[k];
        if (cmd.rfind(":SRA", 0) == 0) {
            const double hours = static_cast<double>(std::atoll(cmd.substr(4, 9).c_str())) / 5400000.0;
            owner = -1;
            for (int i = 0; i < iterations; ++i) {
                if (std::abs(hours - target_ra(i)) < 0.01) {
                    owner = i;
                }
            }
        } else if ((cmd == ":MS1#" || cmd == ":MS2#") && owner >= 0 && k >= log_at_return[owner]) {
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
