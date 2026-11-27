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
// Once it returns, no :MS1# may reach the mount and the stop has been
// sent, even when the cached status says the mount is not slewing.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
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

// Issue #2395 (AB-3): the GOTO dispatch invalidates the status cache, so the AbortSlew right after it reads the mount
// for its parked check. That read failing on a flaky link must not keep the stop off the wire: :Q# goes out first.
TEST_CASE("iOptron AbortSlew - the stop goes out when the status read for the parked check fails",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    driver->slew_to_coordinates_async(6.0, 45.0);
    REQUIRE(wait_for_command(mount, ":MS1#", 1));
    mount.fail_next_reads(1);
    const int stops_before = mount.count(":Q#");
    CHECK_NOTHROW(driver->abort_slew());
    CHECK(mount.count(":Q#") == stops_before + 1);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Issue #2395 (AB-3): InvalidWhileParked still comes from a status read that succeeded and says parked (ConformU
// expects it), after the stop went out.
TEST_CASE("iOptron AbortSlew - a parked mount still raises InvalidWhileParked from a good read",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    mount.set_system_status(6);  // parked
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_at_park());

    const int stops_before = mount.count(":Q#");
    alpacacore::test::require_alpaca_error([&] { driver->abort_slew(); }, alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(mount.count(":Q#") == stops_before + 1);
    CHECK(mount.count(":ST1#") == 0);
    driver->set_connected(false);
}

// Issue #2760: AbortSlew checks the mount stopped (the shared util::run_verified_stop): a still position pair 500 ms
// apart and a status that shows no motion, polled every 250 ms on the task clock, with :Q# sent again as asked, three
// in all within 3 s. FakeIoptronMount::ignore_stops() models a mount that misses stops: while it runs, :GLS# reports
// slewing and every :GEP# read moves the RA by 300 arcsec.
namespace {

constexpr const char* kStopNotVerified = "AbortSlew: the mount still reports motion after 3 stops";

// Steps the fake task clock by the 250 ms verify poll, letting every woken body settle, until done() or `limit` of
// virtual time has passed.
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& done,
                   std::chrono::seconds limit = std::chrono::seconds(8)) {
    for (auto spent = std::chrono::milliseconds(0); spent <= limit; spent += std::chrono::milliseconds(250)) {
        if (done()) return true;
        const auto waits = clock.wait_count();
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(5));
        // The check reads the mount over real I/O between its waits: give it the turn to wait again (or to end)
        // before the next step, or the steps would pass while it is still reading.
        clock.wait_for_wait_count(waits + 1, std::chrono::milliseconds(50));
    }
    return done();
}

void advance_for(alpacacore::test::FakeTaskClock& clock, std::chrono::seconds total) {
    advance_until(clock, [] { return false; }, total);
}

std::string slewing_error(alpacacore::TelescopeDriver& driver) {
    try {
        (void)driver.get_slewing();
        return "";
    } catch (const alpacacore::AlpacaException& e) {
        return e.what();
    }
}

struct RunningMount {
    alpacacore::test::FakeIoptronMount mount{"0012", 0.0};
    alpacacore::test::FakeTaskClock clock;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    int q_before = 0;

    // Connects and starts a GOTO the fake runs until a stop is honoured, then aborts it: the first :Q# is sent.
    void start_and_abort() {
        REQUIRE(mount.ok());
        driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        driver->slew_to_coordinates_async(6.0, 45.0);
        REQUIRE(wait_for_command(mount, ":MS1#", 1));
        REQUIRE(mount.moving());
        q_before = mount.count(":Q#");
        driver->abort_slew();
        REQUIRE(mount.count(":Q#") == q_before + 1);
    }
    int stops() const { return mount.count(":Q#") - q_before; }
};

}  // namespace

// Falsified by: ioptron_telescope_driver.cpp abort_slew() sending one :Q# and no stop check (the mount runs on).
TEST_CASE("iOptron AbortSlew - a stop the mount ignored once is sent again and verified",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.ignore_stops(1);
    rig.start_and_abort();
    CHECK(advance_until(rig.clock, [&] { return !rig.mount.moving(); }));
    advance_for(rig.clock, std::chrono::seconds(4));
    CHECK(rig.stops() == 2);
    CHECK(rig.mount.count(":qR#") == 1);  // the resend carries the axis stops too
    CHECK(rig.mount.count(":qD#") == 1);
    CHECK_FALSE(rig.mount.moving());
    const double stopped_ra = rig.mount.ra_hours();
    advance_for(rig.clock, std::chrono::seconds(2));
    CHECK(rig.mount.ra_hours() == stopped_ra);  // the position the fake holds no longer moves
    CHECK(slewing_error(*rig.driver).empty());
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp abort_slew() not resending the stop (only one :Q# in all).
TEST_CASE("iOptron AbortSlew - a mount that ignores two stops is stopped by the third",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.ignore_stops(2);
    rig.start_and_abort();
    CHECK(advance_until(rig.clock, [&] { return !rig.mount.moving(); }));
    advance_for(rig.clock, std::chrono::seconds(4));
    CHECK(rig.stops() == 3);
    CHECK_FALSE(rig.mount.moving());
    CHECK(slewing_error(*rig.driver).empty());  // verified: no failure kept
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp abort_slew() asking for the stop check only when it found a motion the
// driver owned (a second AbortSlew finds none, and ends the first check).
TEST_CASE("iOptron AbortSlew - a second AbortSlew keeps the stop check",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.status_lies_until_stable();
    rig.mount.ignore_stops(10);
    rig.start_and_abort();
    rig.driver->abort_slew();
    advance_for(rig.clock, std::chrono::seconds(6));
    CHECK(rig.stops() >= 3);  // the second AbortSlew's stop is sent again
    CHECK(rig.mount.moving());
    CHECK(slewing_error(*rig.driver) == kStopNotVerified);
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp abort_slew() never raising the stop-not-verified failure for Slewing.
TEST_CASE("iOptron AbortSlew - a mount that ignores every stop gets 3 :Q# and Slewing raises",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.ignore_stops(10);
    rig.start_and_abort();
    CHECK(advance_until(rig.clock, [&] { return !slewing_error(*rig.driver).empty(); }));
    advance_for(rig.clock, std::chrono::seconds(4));
    CHECK(rig.stops() == 3);  // never a fourth
    CHECK(rig.mount.moving());
    CHECK(slewing_error(*rig.driver) == kStopNotVerified);
    CHECK(slewing_error(*rig.driver) == kStopNotVerified);  // kept until the next command
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp abort_slew() trusting a :GLS# status that shows no motion.
TEST_CASE("iOptron AbortSlew - a status that lies stopped does not verify the stop, the position does",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.status_lies_until_stable();
    rig.mount.ignore_stops(1);
    rig.start_and_abort();
    CHECK(advance_until(rig.clock, [&] { return !rig.mount.moving(); }));
    advance_for(rig.clock, std::chrono::seconds(4));
    CHECK(rig.stops() == 2);
    CHECK_FALSE(rig.mount.moving());
    CHECK(slewing_error(*rig.driver).empty());
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp not reaping the stop check when a new GOTO starts (a late :Q# would end
// it).
TEST_CASE("iOptron AbortSlew - a new GOTO ends the stop check, which sends no more stops",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.ignore_stops(10);
    rig.start_and_abort();
    REQUIRE(advance_until(rig.clock, [&] { return rig.stops() >= 2; }));
    rig.driver->slew_to_coordinates_async(7.0, 45.0);
    REQUIRE(wait_for_command(rig.mount, ":MS1#", 2));
    const int stops = rig.stops();
    advance_for(rig.clock, std::chrono::seconds(5));
    CHECK(rig.stops() == stops);
    CHECK(slewing_error(*rig.driver).empty());  // the old check's failure never reaches the new GOTO
    rig.driver->set_connected(false);
}

// Falsified by: ioptron_telescope_driver.cpp not reaping the stop check at disconnect (it would send :Q# on the closed
// link).
TEST_CASE("iOptron AbortSlew - a disconnect ends the stop check, which sends no more stops",
          "[ioptron][telescope][abort][unit][goto-safety]") {
    RunningMount rig;
    rig.mount.ignore_stops(10);
    rig.start_and_abort();
    REQUIRE(advance_until(rig.clock, [&] { return rig.stops() >= 2; }));
    rig.driver->set_connected(false);
    const int stops = rig.stops();
    advance_for(rig.clock, std::chrono::seconds(5));
    CHECK(rig.stops() == stops);
    CHECK_FALSE(rig.driver->get_connected());
}

#endif
