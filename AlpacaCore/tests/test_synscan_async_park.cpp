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
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeSynScanState {
    std::atomic<bool> goto_seen{false};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    // #742: with `hold_goto` set the next GOTO goes unanswered and sets
    // `goto_held`, which is the test's cue to drop the connection before the
    // GOTO's read times out.
    std::atomic<bool> hold_goto{false};
    std::atomic<bool> goto_held{false};
    std::atomic<unsigned char> model_id{50};
    // With `no_location` set the handset never answers the location query,
    // so the driver has no site unless config or a client supplies one.
    std::atomic<bool> no_location{false};
    std::mutex mutex;
    std::string position = "00000000,00000000#";
    std::vector<std::string> gotos;

    void set_position(std::string value) {
        std::lock_guard<std::mutex> lock(mutex);
        position = std::move(value);
    }

    std::vector<std::string> goto_snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return gotos;
    }

    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (!goto_seen.load()) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return Clock::now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(std::shared_ptr<FakeSynScanState> st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#" (the connect-time link check)
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z': {
                std::lock_guard<std::mutex> lock(st->mutex);
                return st->position;  // parseable 16/24-bit position pair
            }
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (st->hold_goto.load()) {
                    st->goto_held.store(true);
                    return "";
                }
                {
                    std::lock_guard<std::mutex> lock(st->mutex);
                    st->gotos.push_back(chunk);
                }
                st->goto_started.store(Clock::now().time_since_epoch().count());
                st->goto_seen.store(true);
                st->goto_count.fetch_add(1);
                return "#";
            case 'w':
                if (st->no_location.load()) return "";
                return std::string(8, '\0') + "#";
            case 'W':
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->goto_seen.store(false);
                return "#";
            case 'T':
            case 'P':  // tracking mode write / passthrough
                return "#";
            case 'm':  // model id: chr(model) + "#"; 50 = EQM-35 Pro
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    // the connect task holds mutex_ for the next few hundred milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The flag's value mid-task is driver-specific (SynScan raises it before
    // the warm-up queries, see .github/instructions/alpaca-http-conformance.instructions.md
    // on why get_connected() is not a completion signal); the contract under test is that the read returns
    // at once while Connecting is still true.
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

TEST_CASE("SynScan - Name carries the model and the (SynScan) suffix once connected", "[synscan][telescope][async]") {
    // PR #278: a mount reachable over both the hand controller and the
    // direct motor-controller path resolves to the same model string in
    // both drivers; the "(SynScan)" suffix is what tells them apart in a
    // client's device list. Asserted here rather than only on hardware.
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    CHECK(driver->get_name() == "Sky-Watcher Mount (SynScan)");  // no model known yet
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(driver->get_name() == "Sky-Watcher EQM-35 Pro (SynScan)");  // fake answers model id 50
    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
}

TEST_CASE("SynScan async - Park returns immediately, AtPark flips when the slew ends", "[synscan][telescope][async]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 20000));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());  // park stops tracking

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("SynScan SetPark - parked hour angle follows sidereal-time shifts", "[synscan][telescope][park]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_park();                      // mechanical position is RA=0, Dec=0 at longitude 0
    st->set_position("40000000,00000000#");  // six hours later, the same mount position reads RA=6h
    driver->set_site_longitude(90.0);        // longitude +90 degrees has the same six-hour LST effect
    driver->park();
    REQUIRE(wait_until([&] { return st->goto_count.load() > 0; }, 5000));

    const auto gotos = st->goto_snapshot();
    REQUIRE(gotos.size() == 1);
    INFO("Park GOTO after a six-hour sidereal shift: " << gotos.front());
    CHECK((gotos.front() == "r40000000,00000000" || gotos.front() == "R4000,0000"));
    driver->set_connected(false);
}

TEST_CASE("SynScan SetPark - Alt-Az mount retains azimuth and altitude", "[synscan][telescope][park]") {
    auto st = std::make_shared<FakeSynScanState>();
    st->model_id.store(128);  // AZ GOTO
    st->set_position("40000000,20000000#");
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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

TEST_CASE("SynScan ambiguous or unknown mount - Park keeps the RA/Dec fallback", "[synscan][telescope][park]") {
    for (const auto model_id :
         {static_cast<unsigned char>(5), static_cast<unsigned char>(6), static_cast<unsigned char>(255)}) {
        for (const bool set_park_first : {false, true}) {
            auto st = std::make_shared<FakeSynScanState>();
            st->model_id.store(model_id);
            alpacacore::test::FakeMountServer server(synscan_responder(st));
            REQUIRE(server.ok());
            auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
                0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
        auto st = std::make_shared<FakeSynScanState>();  // model 50, EQM-35 Pro: known equatorial
        st->no_location.store(true);
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
    const std::string& cmd = gotos.front();
    const bool precise = cmd.front() == 'r';
    const std::string ra_hex = cmd.substr(1, precise ? 8 : 4);
    const double full_scale = precise ? 4294967296.0 : 65536.0;
    const double ra_hours = static_cast<double>(std::stoul(ra_hex, nullptr, 16)) / full_scale * 24.0;
    CHECK(std::abs(ra_hours - 6.0) < 0.01);
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Unpark during a park cancels it", "[synscan][telescope][async]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // The cancelled park task must never flip AtPark afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    CHECK_THROWS_AS(driver->move_axis(0, 0.5), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->slew_to_coordinates_async(5.0, 20.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->sync_to_coordinates(5.0, 20.0), alpacacore::AlpacaException);
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
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
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
    CHECK(driver->get_slewing());  // the hardware poll fails too, so the cached state stands

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

    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    // A long reply timeout leaves the test ample time to drop the link while
    // the driver still waits on the GOTO, so the stops always meet a closed
    // peer.
    auto info = endpoint(server.port());
    info.response_timeout_ms = 2000;
    auto driver =
        alpacacore::vendor::synscan::create_synscan_telescope(0, info, alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->hold_goto.store(true);
    driver->park();
    // No driver call until the link is down: the park task holds the driver
    // mutex while it waits on the GOTO, so a getter here would block until
    // the GOTO failed and the stops had already gone out on a live link.
    REQUIRE(wait_until([&] { return st->goto_held.load(); }, 5000));
    REQUIRE(server.drop_connections());
    REQUIRE(wait_until([&] { return stop_errors.load() > 0; }, 10000));
    CHECK(stop_errors.load() == 1);
    CHECK_FALSE(driver->get_at_park());
    CHECK(driver->get_slewing());

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
        auto st = std::make_shared<FakeSynScanState>();
        alpacacore::test::FakeMountServer server(synscan_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        std::atomic<int> code{-1};
        std::thread slewer([&] {
            try {
                driver->slew_to_coordinates(5.0, 20.0);
                code.store(0);
            } catch (const AlpacaException& e) {
                code.store(static_cast<int>(e.error_code()));
            }
        });
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO is on the wire
        c.supersede(*driver);
        slewer.join();
        CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        driver->set_connected(false);
    }
}

TEST_CASE("SynScan sync slew - uncontended slew returns normally", "[synscan][telescope][async]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_NOTHROW(driver->slew_to_coordinates(5.0, 20.0));
    CHECK(st->goto_count.load() == 1);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

#endif  // !_WIN32
