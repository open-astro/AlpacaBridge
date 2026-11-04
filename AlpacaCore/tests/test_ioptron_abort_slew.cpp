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

// open-astro#768, iOptron: AbortSlew must fence the async slew slot.
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
#include "driver_rig.h"          // FakeTaskClock
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

bool wait_for_command(const alpacacore::test::FakeIoptronMount& mount, const std::string& cmd, int n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (mount.count(cmd) < n && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return mount.count(cmd) >= n;
}

// Index of the last `cmd` in the mount's log, or -1.
long last_index(const alpacacore::test::FakeIoptronMount& mount, const std::string& cmd) {
    const auto log = mount.commands();
    for (long k = static_cast<long>(log.size()) - 1; k >= 0; --k) {
        if (log[static_cast<std::size_t>(k)] == cmd) return k;
    }
    return -1;
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

// Issue #2000, ITelescopeV4 AbortSlew: "equatorial slews by definition always start and finish with Tracking on". The
// HAE29C reports tracking off for a whole GOTO and :Q# leaves tracking as it is, so the driver turns it back on.
// Falsified by: ioptron_telescope_driver.cpp abort_slew() not setting restore_tracking for an equatorial slew.
TEST_CASE("iOptron AbortSlew - an aborted equatorial GOTO finishes with Tracking on",
          "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);

    driver->slew_to_coordinates_async(6.0, 45.0);
    REQUIRE(wait_for_command(mount, ":MS1#", 1));
    clock.advance(std::chrono::seconds(31));  // past the read-back window and the status TTL
    REQUIRE_FALSE(driver->get_tracking());    // the firmware reports the GOTO as not tracking

    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());
    CHECK(last_index(mount, ":ST1#") > last_index(mount, ":Q#"));
    CHECK(driver->get_tracking());
    clock.advance(std::chrono::seconds(3));  // past the read-back window: the mount itself says tracking
    CHECK(driver->get_tracking());
    // The next equatorial slew is accepted (it requires Tracking).
    CHECK_NOTHROW(driver->slew_to_coordinates_async(7.0, 45.0));
    REQUIRE(wait_for_command(mount, ":MS1#", 2));
    driver->abort_slew();
    driver->set_connected(false);
}

// Issue #2000: an aborted MoveAxis returns Tracking to its state before the motion, as stopping it with rate 0 does.
// Falsified by: ioptron_telescope_driver.cpp abort_slew() not restoring tracking_state_before_move_.
TEST_CASE("iOptron AbortSlew - an aborted MoveAxis returns Tracking to its state before",
          "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    for (const bool before : {true, false}) {
        INFO("Tracking before MoveAxis: " << before);
        driver->set_tracking(before);
        const std::string restore = before ? ":ST1#" : ":ST0#";
        const int restores_before = mount.count(restore);
        driver->move_axis(0, driver->get_axis_rate_ranges(0).front().first);  // the slowest rate
        mount.set_system_status(0);  // the firmware stops tracking during the move
        driver->abort_slew();
        CHECK(mount.count(restore) == restores_before + 1);
        CHECK(last_index(mount, restore) > last_index(mount, ":Q#"));
        CHECK(driver->get_tracking() == before);
        clock.advance(std::chrono::seconds(3));
        CHECK(driver->get_tracking() == before);
    }
    driver->set_connected(false);
}

// Issue #2000: with nothing moving, or after an Alt/Az slew, AbortSlew leaves Tracking as the stop leaves it.
// Falsified by: ioptron_telescope_driver.cpp abort_slew() turning tracking on for every abort.
TEST_CASE("iOptron AbortSlew - with no equatorial motion Tracking is not changed",
          "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(false);
    const int on_before = mount.count(":ST1#");
    const int off_before = mount.count(":ST0#");
    driver->abort_slew();
    CHECK(mount.count(":ST1#") == on_before);
    CHECK(mount.count(":ST0#") == off_before);
    clock.advance(std::chrono::seconds(3));
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

// Issue #2000, review of #2016: an equatorial GOTO that landed and was superseded by an Alt/Az slew must not leave
// AbortSlew turning tracking on: Alt/Az slews need Tracking off (#1377), and the abort leaves it so. Falsified by:
// ioptron_telescope_driver.cpp supersede_slew_dispatch_locked() not clearing equatorial_slew_.
TEST_CASE("iOptron AbortSlew - an aborted Alt/Az slew after an equatorial GOTO sends no tracking command",
          "[ioptron][telescope][abort][unit]") {
    alpacacore::test::FakeIoptronMount mount("0036", 0.0);
    REQUIRE(mount.ok());
    mount.set_gps(0, 40.0, -105.0);  // the stored site the Alt/Az conversion reads
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);

    // An equatorial GOTO that lands (the default fake lands at once) and is seen landing.
    driver->slew_to_coordinates_async(6.0, 45.0);
    REQUIRE(wait_for_command(mount, ":MS1#", 1));
    clock.advance(std::chrono::seconds(31));  // past the slew hold and the status TTL
    REQUIRE_FALSE(driver->get_slewing());

    driver->set_tracking(false);
    mount.set_system_status(0);
    driver->slew_to_alt_az_async(45.0, 200.0);
    REQUIRE(wait_for_command(mount, ":MS1#", 2));
    const int on_before = mount.count(":ST1#");
    const int off_before = mount.count(":ST0#");
    driver->abort_slew();
    CHECK(mount.count(":ST1#") == on_before);
    CHECK(mount.count(":ST0#") == off_before);
    clock.advance(std::chrono::seconds(3));
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

#endif
