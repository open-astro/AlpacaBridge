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
//
// Park is an asynchronous initiator (issue #208): it must return well inside
// the ConformU 4.5 STANDARD 1 s target while the park slew runs in the
// background, Slewing stays true until the mount arrives, and AtPark flips
// true in the same step Slewing drops. A FakeMountServer plays a SynScan
// handset whose GOTO takes ~1.5 s, so the whole lifecycle runs hardware-free.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_synscan_handset.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

using alpacacore::test::FakeSynScanHandset;
using alpacacore::test::SynScanGotoLanding;
using alpacacore::test::wait_until;

// This file's handset: a GOTO lands on its aim, and the mount starts at RA 0, Dec 0.
std::shared_ptr<FakeSynScanHandset> make_handset(
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    return std::make_shared<FakeSynScanHandset>(clock, SynScanGotoLanding::OnTarget, "00000000,00000000#");
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

// RA in hours of a SynScan GOTO command: "r%08X,%08X" (precise) or "R%04X,%04X".
double goto_ra_hours(const std::string& cmd) {
    const bool precise = cmd.front() == 'r';
    const std::string ra_hex = cmd.substr(1, precise ? 8 : 4);
    const double full_scale = precise ? 4294967296.0 : 65536.0;
    return static_cast<double>(std::stoul(ra_hex, nullptr, 16)) / full_scale * 24.0;
}

// SetPark saves an hour angle against the sidereal clock's LST and Park converts
// it back against the same LST. The callers pin that clock through
// set_sidereal_wall_clock_for_testing(), so after a six-hour shift the RA target
// is 6 h to within one count of the command's resolution (24 bits for "r", 16
// for "R"). An unpinned host clock drifted it by a real second (r4000C300) when
// the two calls straddled a second boundary (#976). Dec is not time-dependent
// and stays exact.
void check_six_hour_park_goto(const std::string& cmd) {
    CHECK(cmd.substr(cmd.find(',')) == (cmd.front() == 'r' ? ",00000000" : ",0000"));
    const double resolution_hours = 24.0 / (cmd.front() == 'r' ? 16777216.0 : 65536.0);
    const double past_six = goto_ra_hours(cmd) - 6.0;
    INFO("RA target past 6 h: " << past_six * 3600.0 << " s");
    CHECK(past_six >= -resolution_hours);
    CHECK(past_six <= resolution_hours);
}

// Steps the fake clock one `step` at a time, once the driver's task is parked
// on it, until `pred` holds or `max_steps` steps pass. Each step waits for the
// woken task to settle, so no step lands between two of its waits.
bool advance_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                   std::chrono::milliseconds step = std::chrono::milliseconds(250), int max_steps = 200) {
    for (int i = 0; i < max_steps; ++i) {
        if (pred()) return true;
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return pred();
}

}  // namespace

TEST_CASE("SynScan - get_connected() answers at once while a connect is in flight", "[synscan][telescope][async]") {
    // set_connected(true) holds the driver mutex across every handshake round
    // trip, and the router polls get_connected() throughout (the PUT connected
    // wait, every GET connected). With a mutex-taking getter those calls
    // blocked for the whole connect and the router's deadline never fired
    // (issue #130). Stall the firmware query so the mutex is held for a while
    // and prove the getter still returns immediately.
    static constexpr int kStallMs = 400;  // static: odr-used inside the lambda below
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#"
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'V':
                // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task
                // clock.
                std::this_thread::sleep_for(std::chrono::milliseconds(kStallMs));
                return "042A00#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            default:
                return "0#";
        }
    });
    REQUIRE(server.ok());
    auto info = endpoint(server.port());
    info.response_timeout_ms = 1000;  // longer than the stall: the firmware query must succeed, not time out
    auto driver =
        alpacacore::vendor::synscan::create_synscan_telescope(0, info, alpacacore::vendor::synscan::SynScanVersion::V4);

    driver->connect();
    REQUIRE(wait_until([&] { return driver->get_connecting(); }, 1000));
    // Past the port open and the echo, inside the stalled firmware query:
    // the connect task is in the handshake for the next few hundred milliseconds; it holds transition_mutex_ and
    // never mutex_ across an exchange.
    // Real time: the fake mount and the test's own polling run on the host clock, not the driver's task clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The flag's value mid-task is driver-specific (SynScan raises it last, after the first frame is held, see
    // .github/instructions/alpaca-http-conformance.instructions.md on why get_connected() is not a completion
    // signal); the contract under test is that the read returns at once while Connecting is still true.
    const auto t0 = Clock::now();
    static_cast<void>(driver->get_connected());
    const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(read_ms < 100);
    CHECK(driver->get_connecting());

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_connected(false);
}

TEST_CASE("SynScan - a silent handset fails the connect instead of reporting a phantom link",
          "[synscan][telescope][async]") {
    // connect() only opens the port. Before the echo gate a link with nothing
    // listening came up as Connected=true once every handshake query had
    // burnt its full response timeout (all swallowed), and every command then
    // timed out too. Now the echo is the first thing on the wire and its
    // silence fails the connect within a single timeout.
    auto queries = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([queries](const std::string&) -> std::string {
        queries->fetch_add(1);
        return "";  // nothing is ever sent back
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);  // 200 ms response timeout

    const auto t0 = Clock::now();
    driver->connect();
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 5000));
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK_FALSE(driver->get_connected());
    CHECK(connect_ms < 1000);     // one echo timeout, not five swallowed query timeouts
    CHECK(queries->load() == 1);  // only the echo went out
}

TEST_CASE("SynScan - a garbled echo reply recovers on retry", "[synscan][telescope][async]") {
    // A real handset that answers the echo wrong ONCE (a single garbled
    // byte, not silence and not a different device) must not be treated as
    // "not a handset" - the one retry in echo_test() exists for exactly
    // this case, so a momentary line glitch doesn't fail a real connect.
    auto echo_attempts = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([echo_attempts](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K': {
                const int attempt = echo_attempts->fetch_add(1);
                if (attempt == 0) {
                    return "X#";  // wrong byte, first attempt only
                }
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            }
            case 'V':
                return "042A00#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            default:
                return "0#";
        }
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);

    driver->connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(echo_attempts->load() == 2);  // one wrong reply, one retry that matched
    driver->set_connected(false);
}

TEST_CASE("SynScan - a stale reply queued ahead of the echo does not fail the connect", "[synscan][telescope][async]") {
    // Right after the port opens, the first '#'-terminated token on the line
    // can be a reply to a command the PREVIOUS session never read (abrupt
    // service restart mid-poll) with the handset's answer to our echo queued
    // right behind it. echo_test() must read past the stale token within its
    // response timeout and accept the echo - not burn its retry on it, and
    // not fail a healthy handset (PR #3 review). Both tokens arrive in one
    // write here, the worst case for a first-token reader.
    auto echo_attempts = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([echo_attempts](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                echo_attempts->fetch_add(1);
                return std::string("12AB0500,20000500#") + std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'V':
                return "042A00#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            default:
                return "0#";
        }
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);

    driver->connect();
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(echo_attempts->load() == 1);  // the stale token was read past, not retried around
    driver->set_connected(false);
}

TEST_CASE("SynScan - a persistently garbled echo fails the connect rather than proceeding",
          "[synscan][telescope][async]") {
    // Two wrong-but-framed replies in a row must fail the connect, not be
    // accepted as "probably a handset, continuing" - accepting an unverified
    // reply here is what let a merely-noisy port (something answering SOME
    // '#'-terminated bytes, not necessarily a handset) proceed into the
    // firmware/model/site queries that follow and get individually
    // swallowed, reproducing the original "Connected=true, then every
    // command times out" bug for a narrower trigger (garbled echo instead
    // of total silence) - the exact gap a code review caught on PR #3.
    auto queries = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([queries](const std::string& chunk) -> std::string {
        queries->fetch_add(1);
        if (!chunk.empty() && chunk[0] == 'K') {
            return "X#";  // always wrong, never the echoed byte
        }
        return "0#";
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);

    const auto t0 = Clock::now();
    driver->connect();
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 5000));
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK_FALSE(driver->get_connected());
    CHECK(connect_ms < 1000);     // two quick mismatched replies, not five swallowed query timeouts
    CHECK(queries->load() == 2);  // only the two echo attempts - never firmware/model/site
}

TEST_CASE("SynScan - Name carries the model once connected", "[synscan][telescope][async]") {
    // PR #278: a mount reachable over both the hand controller and the
    // direct motor-controller path resolves to the same model string in
    // both drivers. Decision 0024 gives both the same Name; the Description
    // tells them apart.
    auto st = make_handset();
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    CHECK(driver->get_name() == "Sky-Watcher Telescope");  // no model known yet
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_name() == "Sky-Watcher EQM-35 Pro");  // fake answers model id 50
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
}

// Decision 0024. Falsified by: synscan_telescope_driver.cpp synscan_model_id_to_name() returning "Mount (ID n)" for an
// id not in its table.
TEST_CASE("SynScan - an unknown model id is named Sky-Watcher Telescope", "[synscan][telescope][async]") {
    auto st = make_handset();
    st->model_id.store(77);  // in no row of the V3/V4 table
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_name() == "Sky-Watcher Telescope");
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
}

TEST_CASE("SynScan async - Park returns immediately, AtPark flips when the slew ends", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_at_park());

    const auto t0 = Clock::now();
    driver->park();
    const auto park_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(park_ms < 1000);           // ConformU 4.5 STANDARD target for an async initiator
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO was dispatched
    // Park while a park is in flight is a no-op: the running slew is neither
    // cancelled nor restarted (still exactly one GOTO on the wire).
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_at_park());
    // The task is parked on its first poll: a second GOTO would be on the wire by now.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(advance_until(clock, [&] { return driver->get_at_park(); }));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());  // park stops tracking

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Park survives dropped status replies below the threshold", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->drop_slew_status.store(2);
    REQUIRE(advance_until(clock, [&] { return driver->get_at_park(); }));
    CHECK(st->drop_slew_status.load() == 0);  // both drops were consumed by Park polls
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Park failure count resets on a good status reply", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->goto_never_ends.store(true);
    st->drop_slew_status.store(2);
    REQUIRE(advance_until(clock, [&] { return st->drop_slew_status.load() == 0; }));
    // A good reply that still reports the slew in progress: the failure count must go back to zero here.
    const int polls_after_drops = st->slew_status_polls.load();
    REQUIRE(advance_until(clock, [&] { return st->slew_status_polls.load() > polls_after_drops; }));
    REQUIRE_FALSE(driver->get_at_park());
    st->drop_slew_status.store(2);
    const int polls_before_second_drops = st->slew_status_polls.load();
    REQUIRE(advance_until(clock, [&] { return st->drop_slew_status.load() == 0; }));
    CHECK(st->slew_status_polls.load() >= polls_before_second_drops + 2);
    // Two failures on each side of the good reply: four in all, never three in a row.
    st->goto_never_ends.store(false);
    REQUIRE(advance_until(clock, [&] { return driver->get_at_park(); }));
    driver->set_connected(false);
}

TEST_CASE("SynScan async - a new Park starts its failure count at zero", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->drop_slew_status.store(1000);
    bool park_failed = false;
    REQUIRE(advance_until(clock, [&] {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException& ex) {
            // The poll's fault may precede the park body's own error; the failure is the stored one.
            if (std::string(ex.what()).find("communications compromised") != std::string::npos) return false;
            park_failed = true;
            return true;
        }
    }));
    REQUIRE(park_failed);
    st->drop_slew_status.store(0);
    st->goto_seen.store(false);
    // The second Park meets one dropped reply; a count left at three would fail it at once.
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->drop_slew_status.store(1);
    // The first Park's fault stays latched until a poll is answered, and AtPark refuses meanwhile.
    REQUIRE(advance_until(clock, [&] {
        try {
            return driver->get_at_park();
        } catch (const alpacacore::AlpacaException&) {
            return false;
        }
    }));
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Park fails after three consecutive dropped status replies", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    // The dropped-reply count below is the park body's own status polls: the status poll's `L` reads would consume
    // drops too.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    st->drop_slew_status.store(1000);
    // The failed Park surfaces on the next Slewing read as its stored error.
    bool park_failed = false;
    REQUIRE(advance_until(clock, [&] {
        try {
            return !driver->get_slewing();
        } catch (const alpacacore::AlpacaException&) {
            park_failed = true;
            return true;
        }
    }));
    CHECK(park_failed);
    CHECK_FALSE(driver->get_at_park());
    CHECK(st->drop_slew_status.load() == 1000 - 3);  // exactly three polls before failing
    driver->set_connected(false);
}

TEST_CASE("SynScan SetPark - parked hour angle follows sidereal-time shifts", "[synscan][telescope][park]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // Pinned sidereal wall time: a real second between the two calls moved the target by 0xC300 counts (#976).
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
    alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });

    driver->set_park();                      // mechanical position is RA=0, Dec=0 at longitude 0
    st->set_position("40000000,00000000#");  // six hours later, the same mount position reads RA=6h
    driver->set_site_longitude(90.0);        // longitude +90 degrees has the same six-hour LST effect
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

    const auto gotos = st->goto_snapshot();
    REQUIRE(gotos.size() == 1);
    INFO("Park GOTO after a six-hour sidereal shift: " << gotos.front());
    check_six_hour_park_goto(gotos.front());
    driver->set_connected(false);
}

TEST_CASE("SynScan SetPark - Alt-Az mount retains azimuth and altitude", "[synscan][telescope][park]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->model_id.store(128);  // AZ GOTO
    st->set_position("40000000,20000000#");
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_park();
    st->set_position("00000000,00000000#");
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

    const auto gotos = st->goto_snapshot();
    REQUIRE(gotos.size() == 1);
    INFO("Alt-Az Park GOTO: " << gotos.front());
    CHECK((gotos.front() == "b40000000,20000000" || gotos.front() == "B4000,2000"));
    driver->set_connected(false);
}

TEST_CASE("SynScan SetPark - AZ-EQ park follows the configured alignment mode (#860)", "[synscan][telescope][park]") {
    using alpacacore::vendor::synscan::SynScanAlignmentSetting;
    {
        // Alt-Az: the park keeps azimuth and altitude, as on an AZ GOTO mount.
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->model_id.store(5);  // AZ-EQ6
        st->set_position("40000000,20000000#");
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, SynScanAlignmentSetting::AltAz, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        driver->set_park();
        st->set_position("00000000,00000000#");
        driver->park();
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

        const auto gotos = st->goto_snapshot();
        REQUIRE(gotos.size() == 1);
        INFO("AZ-EQ Alt-Az Park GOTO: " << gotos.front());
        CHECK((gotos.front() == "b40000000,20000000" || gotos.front() == "B4000,2000"));
        driver->set_connected(false);
    }
    {
        // Equatorial: the park keeps the hour angle, so a sidereal shift moves the RA target.
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->model_id.store(6);  // AZ-EQ5
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, SynScanAlignmentSetting::Equatorial, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        // Pin the sidereal wall time (a real clock passing a second between the two calls moved the target by
        // 0xC300 counts, #976), so the 90 degree longitude shift is the only sidereal change the target follows.
        const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
        alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });

        driver->set_park();                      // mechanical position is RA=0, Dec=0 at longitude 0
        st->set_position("40000000,00000000#");  // six hours later, the same mount position reads RA=6h
        driver->set_site_longitude(90.0);        // longitude +90 degrees has the same six-hour LST effect
        driver->park();
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

        const auto gotos = st->goto_snapshot();
        REQUIRE(gotos.size() == 1);
        INFO("AZ-EQ equatorial Park GOTO after a six-hour sidereal shift: " << gotos.front());
        check_six_hour_park_goto(gotos.front());
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan ambiguous or unknown mount - Park keeps the RA/Dec fallback", "[synscan][telescope][park]") {
    for (const auto model_id :
         {static_cast<unsigned char>(5), static_cast<unsigned char>(6), static_cast<unsigned char>(255)}) {
        for (const bool set_park_first : {false, true}) {
            alpacacore::test::FakeTaskClock clock;
            auto st = make_handset(clock);
            st->model_id.store(model_id);
            alpacacore::test::FakeMountServer server(st->responder());
            REQUIRE(server.ok());
            auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
                0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
                std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
            REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
            if (set_park_first) {
                CHECK_NOTHROW(driver->set_park());
            }
            CHECK_NOTHROW(driver->park());
            REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
            driver->set_connected(false);
        }
    }
}

TEST_CASE("SynScan equatorial mount without a site - Park keeps the RA/Dec fallback", "[synscan][telescope][park]") {
    for (const bool set_park_first : {false, true}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);  // model 50, EQM-35 Pro: known equatorial
        st->no_location.store(true);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        if (set_park_first) {
            CHECK_NOTHROW(driver->set_park());
        }
        CHECK_NOTHROW(driver->park());
        REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan hour-angle park after a reconnect without a site - Park uses the last known longitude",
          "[synscan][telescope][park]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->set_park());  // RA=0 at longitude 0: saved as an hour angle
    driver->set_site_longitude(90.0);     // the last known longitude is now +90 degrees
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));

    st->no_location.store(true);  // the reconnected handset reports no site
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK_NOTHROW(driver->park());
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

    // Same mechanical position at +90 degrees longitude reads RA=6h, plus the
    // sidereal time the reconnect took (an RA/Dec fallback would read 0h).
    const auto gotos = st->goto_snapshot();
    REQUIRE(gotos.size() == 1);
    INFO("Park GOTO after a reconnect without a site: " << gotos.front());
    CHECK(std::abs(goto_ra_hours(gotos.front()) - 6.0) < 0.01);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Unpark during a park cancels it", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // The cancelled park task must never flip AtPark afterwards.
    // Virtual time well past the GOTO and the start grace.
    CHECK_FALSE(advance_until(clock, [&] { return driver->get_at_park(); }, std::chrono::milliseconds(250), 12));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    alpacacore::test::require_alpaca_error([&] { driver->move_axis(0, 0.5); },
                                           alpacacore::AlpacaError::InvalidWhileParked);
    alpacacore::test::require_alpaca_error([&] { driver->slew_to_coordinates_async(5.0, 20.0); },
                                           alpacacore::AlpacaError::InvalidWhileParked);
    alpacacore::test::require_alpaca_error([&] { driver->sync_to_coordinates(5.0, 20.0); },
                                           alpacacore::AlpacaError::InvalidWhileParked);
    try {
        driver->move_axis(0, 0.5);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    REQUIRE(driver->get_slewing());  // the park is still in flight
    driver->abort_slew();            // ...but AbortSlew may cancel it
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

// #742: the three stops Unpark sends to cancel a park in flight (cancel GOTO,
// then both axes to rate 0) sat in one empty catch, and Unpark then set
// Slewing false and returned success. The SynScan stops are blind sends, so a
// silent handset cannot fail them; the fake resets the link instead (every stop
// after that fails).
// Unpark must throw once the park task is joined, and Slewing must not read
// false: the park slew may still be running.
TEST_CASE("SynScan async - Unpark reports stops it could not send", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the park slew is on the wire
    REQUIRE(server.drop_connections());

    try {
        driver->unpark();
        FAIL("Unpark returned success although the stops could not be sent");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).rfind("Unpark could not stop the park slew: ", 0) == 0);
    }
    CHECK_FALSE(driver->get_at_park());
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Slewing must surface the failed Unpark stop");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Unpark could not stop") != std::string::npos);
    }

    driver->set_connected(false);
}

// #742: when the park task fails (here the GOTO goes unanswered and the link
// drops) it stops the mount; those stops were swallowed and Slewing set false.
// A stop that fails must be logged at ERROR, and Slewing must not read false
// while the mount cannot be reached.
TEST_CASE("SynScan async - a failed park logs the stops it could not send", "[synscan][telescope][async]") {
    std::atomic<int> stop_errors{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Error && component == "SynScan" &&
                message.find("stop after park failure failed: ") != std::string_view::npos &&
                message.find("the mount may still be moving") != std::string_view::npos) {
                ++stop_errors;
            }
        });

    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    // A long reply timeout leaves the test ample time to drop the link while
    // the driver still waits on the GOTO, so the stops always meet a closed
    // peer.
    auto info = endpoint(server.port());
    info.response_timeout_ms = 2000;
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, info, alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    driver->park();
    // No driver call until the link is down: the park task holds the driver
    // mutex while it waits on the GOTO, so a getter here would block until
    // the GOTO failed and the stops had already gone out on a live link.
    REQUIRE(wait_until([&] { return st->goto_held.load(); }, 5000));
    REQUIRE(server.drop_connections());
    REQUIRE(wait_until([&] { return stop_errors.load() > 0; }, 10000));
    CHECK(stop_errors.load() == 1);
    CHECK_FALSE(driver->get_at_park());
    try {
        static_cast<void>(driver->get_slewing());
        FAIL("Slewing must surface the failed Park");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("stopping after failure failed") != std::string::npos);
    }

    driver->set_connected(false);
}

TEST_CASE("SynScan async - malformed Park status reply persists through Slewing",
          "[synscan][telescope][async][parkfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    st->invalid_slew_status.store(true);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->park());
    bool surfaced = false;
    for (int i = 0; i < 48 && !surfaced; ++i) {
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
        try {
            (void)driver->get_slewing();
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            // The poll's own fault latches first (three bad replies of its own); the stored Park error takes
            // precedence over it once the Park has failed, and carries the reply the handset sent.
            const std::string text = ex.what();
            if (text.find("communications compromised") == std::string::npos) {
                CHECK(text.find("Invalid response to SynScan slew-status query") != std::string::npos);
                surfaced = true;
            }
        }
    }
    REQUIRE(surfaced);
    CHECK(st->slew_status_polls.load() > 0);
    // The three bad replies also latched the link fault (decision 0009 rule 1): AtPark refuses with its text.
    alpacacore::test::require_alpaca_error([&] { (void)driver->get_at_park(); },
                                           alpacacore::AlpacaError::DriverException);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - a Park tracking-stop failure persists through Slewing",
          "[synscan][telescope][async][parkfailure]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->park());
    st->reject_tracking_stop.store(true);
    bool surfaced = false;
    for (int i = 0; i < 48 && !surfaced; ++i) {
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
        try {
            (void)driver->get_slewing();
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("stopping tracking failed") != std::string::npos);
            surfaced = true;
        }
    }
    REQUIRE(surfaced);
    CHECK(st->slew_status_polls.load() > 0);
    CHECK(st->tracking_stop_attempts.load() > 0);
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

// #832: a synchronous SlewToCoordinates must notice another client taking the
// mount (AbortSlew, Park, MoveAxis) and throw InvalidOperation instead of
// returning success.
TEST_CASE("SynScan sync slew - superseded by AbortSlew, Park or MoveAxis throws InvalidOperation",
          "[synscan][telescope][async]") {
    using alpacacore::AlpacaException;
    struct Case {
        const char* name;
        std::function<void(alpacacore::TelescopeDriver&)> supersede;
    };
    const Case cases[] = {
        {"AbortSlew", [](alpacacore::TelescopeDriver& d) { d.abort_slew(); }},
        {"Park", [](alpacacore::TelescopeDriver& d) { d.park(); }},
        {"MoveAxis", [](alpacacore::TelescopeDriver& d) { d.move_axis(0, 0.5); }},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        std::atomic<int> code{-1};
        std::atomic<bool> done{false};
        std::thread slewer([&] {
            try {
                driver->slew_to_coordinates(5.0, 20.0);
                code.store(0);
            } catch (const AlpacaException& e) {
                code.store(static_cast<int>(e.error_code()));
            }
            done.store(true);
        });
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO is on the wire
        c.supersede(*driver);
        // The slewer sleeps on the task clock between polls; step it until it has noticed.
        const bool finished = advance_until(clock, [&] { return done.load(); });
        slewer.join();
        REQUIRE(finished);
        CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan sync slew - uncontended slew returns normally", "[synscan][telescope][async]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread slewer([&] {
        try {
            driver->slew_to_coordinates(5.0, 20.0);
        } catch (const std::exception&) {
            threw.store(true);
        }
        done.store(true);
    });
    const bool finished = advance_until(clock, [&] { return done.load(); });
    slewer.join();
    REQUIRE(finished);
    REQUIRE_FALSE(threw.load());
    CHECK(st->goto_count.load() == 1);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

namespace {
std::unique_ptr<alpacacore::TelescopeDriver> mk(int port, alpacacore::util::TaskClock& clock) {
    return alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(port), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
}
}  // namespace

// Case A: saved park Dec 0; mount reads Dec +30 deg; GOTO acked, mount never moves.
TEST_CASE("SynScan case A - refused park GOTO must not report AtPark", "[park][synscan]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = mk(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
    alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });
    driver->set_park();                      // RA 0, Dec 0
    st->set_position("00000000,0AAAAAAB#");  // Dec +30 deg
    st->no_motion.store(true);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
    alpacacore::test::require_alpaca_error([&] { (void)driver->get_slewing(); },
                                           alpacacore::AlpacaError::DriverException);
    bool at_park = false;
    try {
        at_park = driver->get_at_park();
    } catch (const alpacacore::AlpacaException&) {
    }
    CHECK(st->goto_count.load() >= 1);
    CHECK_FALSE(at_park);
    driver->set_connected(false);
}

TEST_CASE("SynScan case B - already at park target still parks", "[park][synscan]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = mk(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
    alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });
    driver->set_park();
    st->no_motion.store(true);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
    bool at_park = false;
    try {
        at_park = driver->get_at_park();
    } catch (const alpacacore::AlpacaException&) {
    }
    CHECK(at_park);
    driver->set_connected(false);
}

// #1105: the park GOTO runs no landing refinement, so the handset's far miss gets one repeat GOTO here.
TEST_CASE("SynScan - a park GOTO the handset ends 49 arcmin off is repeated once and parks", "[park][synscan]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = mk(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
    alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });
    driver->set_park();
    st->miss_gotos.store(1);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
    CHECK(driver->get_at_park());
    CHECK(st->goto_count.load() == 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan - a park that misses by 49 arcmin twice fails", "[park][synscan]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = mk(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const auto wall = std::chrono::system_clock::time_point{std::chrono::seconds(1800000000)};
    alpacacore::vendor::synscan::set_sidereal_wall_clock_for_testing(*driver, [wall] { return wall; });
    driver->set_park();
    st->miss_gotos.store(2);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
    alpacacore::test::require_alpaca_error([&] { (void)driver->get_slewing(); },
                                           alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(driver->get_at_park());
    CHECK(st->goto_count.load() == 2);
    driver->set_connected(false);
}

// An AZ GOTO mount parks in alt/az: the check compares the alt/az reply with the saved azimuth and altitude.
TEST_CASE("SynScan - an alt-az park the mount does not reach fails and one it reaches parks", "[park][synscan]") {
    for (const bool moves : {false, true}) {
        alpacacore::test::FakeTaskClock clock;
        auto st = make_handset(clock);
        st->model_id.store(128);  // AZ GOTO
        st->set_position("40000000,20000000#");
        alpacacore::test::FakeMountServer server(st->responder());
        REQUIRE(server.ok());
        auto driver = mk(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_park();  // az 90, alt 45
        st->set_position("00000000,00000000#");
        st->no_motion.store(!moves);
        driver->park();
        REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
        if (moves) {
            CHECK(driver->get_at_park());
        } else {
            alpacacore::test::require_alpaca_error([&] { (void)driver->get_slewing(); },
                                                   alpacacore::AlpacaError::DriverException);
            CHECK_FALSE(driver->get_at_park());
        }
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan - a park whose position check cannot read the mount fails without a repeat GOTO",
          "[park][synscan]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = make_handset(clock);
    alpacacore::test::FakeMountServer server(st->responder());
    REQUIRE(server.ok());
    auto driver = mk(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_park();
    st->silent_position.store(true);
    driver->park();
    REQUIRE(advance_until(clock, [&] { return alpacacore::test::park_done(*driver); }));
    alpacacore::test::require_alpaca_error([&] { (void)driver->get_slewing(); },
                                           alpacacore::AlpacaError::DriverException);
    // Whether the three unread positions have latched the link fault yet is timing: AtPark is false, or the fault.
    try {
        const bool at_park = driver->get_at_park();  // a CHECK around the call would record the throw as a failure
        CHECK_FALSE(at_park);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

#endif  // !_WIN32
