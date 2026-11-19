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

// Connect/disconnect/operate concurrency stress for the Celestron NexStar
// telescope (audit follow-up, 3.0.1). The audited detached-thread fixes on
// this driver (async slew + pulse-guide threads racing set_connected(false)
// and destruction) only run on a CONNECTED driver, so these tests connect
// through a FakeMountServer playing a handset that accepts a GOTO (issue
// #1859): it reports the mount aligned ('J') and tracking ('t'), answers the
// GOTO and says the slew runs for a while, so a slew or pulse guide really
// starts its slot body. Each case requires the connect and one accepted slew
// and pulse before it storms or destroys. Operations racing a disconnect are
// EXPECTED to throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

using alpacacore::AlpacaDriver;

namespace {

alpacacore::vendor::celestron::ConnectionInfo celestron_endpoint(int port) {
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;  // the replies below end each read at once; a timeout means a missing answer
    return info;
}

using Clock = std::chrono::steady_clock;

// What the handset saw. A GOTO keeps the mount "slewing" for kGotoDuration of
// real time after it, so the slew body is still polling when a Disconnect
// lands; the driver runs on the default (real) task clock here.
struct Handset {
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    std::atomic<int> gotos{0};
    std::atomic<Clock::rep> goto_at{0};
    bool goto_in_progress() const {
        return gotos.load() > 0 && Clock::now() - Clock::time_point(Clock::duration(goto_at.load())) < kGotoDuration;
    }
    void note_goto() {
        goto_at.store(Clock::now().time_since_epoch().count());
        gotos.fetch_add(1);
    }
};

// The handset of test_celestron_async_park.cpp, cut down to what a slew and a
// pulse guide need: aligned ('J'), tracking EQ North ('t'), a parseable
// position, an acknowledged GOTO on both the HC and the AUX pass-through path,
// and every AUX bus probe answered so the autoguider port is found. A reply
// longer than the read it answers is left in the stream for the next read, so
// each command gets a reply of its own length.
alpacacore::test::FakeMountServer::Responder celestron_responder(std::shared_ptr<Handset> handset) {
    return [handset](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";  // parseable 16/24-bit position pair
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                handset->note_goto();
                return "#";
            case 'L':  // GOTO in progress
                return handset->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel GOTO
                return "#";
            case 'J':  // alignment complete: the slew-safety gate
                return "1#";
            case 'w':  // site read: eight binary bytes (a short answer would wait out the response timeout)
                return std::string(8, '\0') + "#";
            case 'W':
            case 'T':
                return "#";
            case 't':  // tracking mode EQ North: equatorial slews require Tracking
                return std::string("\x02#");
            case 'P': {  // AUX pass-through: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                if (op == 0xFE) {  // the binary firmware reply for each AUX bus probe
                    return std::string("\x00\x00#", 3);
                }
                if (op == 0x02 || op == 0x17) {  // MC_GOTO_FAST / MC_GOTO_SLOW
                    handset->note_goto();
                    return "#";
                }
                if (op == 0x13) {  // MC_SLEW_DONE: 0x00 = still slewing, 0xFF = done
                    return handset->goto_in_progress() ? std::string("\x00#", 2) : std::string("\xFF#");
                }
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
}

// Connects, then starts one slew and one pulse guide and requires that each
// reached its slot body and the slew reached the handset. The pulse is longer
// than one 2.55 s MC_AUX_GUIDE chunk, so its chunk chain runs in the pulse slot. Without this a
// handset that refuses the GOTO left every storm on the refusal path (#1859).
void require_slew_and_pulse_bodies(alpacacore::TelescopeDriver& scope, const Handset& handset,
                                   const alpacacore::test::SlotBodyTracker& slews,
                                   const alpacacore::test::SlotBodyTracker& pulses) {
    REQUIRE(alpacacore::test::settle_connected(scope, true, std::chrono::seconds(10)));
    const int gotos = handset.gotos.load();
    const int slew_bodies = slews.started();
    const int pulse_bodies = pulses.started();
    scope.slew_to_coordinates_async(5.0, 20.0);
    scope.pulse_guide(0, 4000);
    REQUIRE(slews.started() > slew_bodies);
    REQUIRE(pulses.started() > pulse_bodies);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (handset.gotos.load() == gotos && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // real time: the GOTO exchange on the socket
    }
    REQUIRE(handset.gotos.load() > gotos);
}

void telescope_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { scope.set_target_right_ascension(5.0); });
    guard([&] { scope.set_target_declination(20.0); });
    guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_target_declination()); });
    guard([&] { static_cast<void>(scope.get_guide_rate()); });
    guard([&] { scope.set_guide_rate({0.004, 0.004}); });
    guard([&] { scope.slew_to_target_async(); });
    guard([&] { scope.sync_to_target(); });
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_declination()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    guard([&] { static_cast<void>(scope.get_is_pulse_guiding()); });
    guard([&] { static_cast<void>(scope.get_at_home()); });
    guard([&] { static_cast<void>(scope.get_at_park()); });
    guard([&] { static_cast<void>(scope.get_declination_rate()); });
    guard([&] { static_cast<void>(scope.get_right_ascension_rate()); });
    guard([&] { static_cast<void>(scope.get_tracking_rate()); });
    // The newly-fixed thread paths: async slew and pulse guiding issued
    // while the lifecycle threads disconnect underneath them.
    guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
    guard([&] { scope.pulse_guide(0, 50); });
    guard([&] { scope.abort_slew(); });
}

}  // namespace

TEST_CASE("Celestron telescope - concurrent connect/disconnect/slew/pulse stress", "[celestron][telescope][stress]") {
    auto handset = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server(celestron_responder(handset));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(server.port()));
    alpacacore::test::SlotBodyTracker slews;
    alpacacore::test::SlotBodyTracker pulses;
    alpacacore::vendor::celestron::set_slew_spawn_for_testing(*driver, slews.spawn());
    alpacacore::vendor::celestron::set_pulse_spawn_for_testing(*driver, pulses.spawn());

    // The storm reaches the slew and pulse bodies (#1859): a connected driver
    // accepts one of each, and a Disconnect cancels and joins both before it
    // returns, the GOTO still "slewing" and the 4 s pulse still chaining.
    require_slew_and_pulse_bodies(*driver, *handset, slews, pulses);
    driver->set_connected(false);
    CHECK(slews.running() == 0);
    CHECK(pulses.running() == 0);

    // open-astro#326: one guard per call, replacing no isolation at all --
    // run_lifecycle_stress wraps the WHOLE callback in one try/catch, so a
    // storm racing a disconnect exercised get_tracking() and skipped the six
    // calls below it, including the async slew and pulse-guide paths this
    // registration exists to storm.
    //
    // The set REPLACES the default {NotConnected}. This registration runs
    // CONNECTED over FakeMountServer, so a live driver's operate callback
    // legitimately throws more than that: InvalidValue and InvalidOperation
    // from a slew or pulse racing a park or another motion, ValueNotSet from
    // a target read before its write, and the shared NotImplemented code.
    //
    // DriverException is not in the set: the handset above answers every
    // command the storm sends, so a failed exchange is a code outside this
    // set, and guard.report() names every distinct one it saw. (With the
    // canned "00000000,00000000#" handset before #1859 the driver reported
    // "Invalid status response from mount" and the set had to name it.)
    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::ValueNotSet,
        alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { telescope_operate(guard, d); });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and a bare CHECK would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Celestron telescope - destruction races an in-flight connect", "[celestron][telescope][stress]") {
    alpacacore::test::FakeMountServer server(celestron_responder(std::make_shared<Handset>()));
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(port)); },
        // 25 iterations, not the default 100: each destruction joins the
        // in-flight connect, whose handshake and AUX bus probes are many
        // exchanges with the handset.
        25);
}

TEST_CASE("Celestron telescope - destruction mid-operation (slew/pulse threads live)",
          "[celestron][telescope][stress]") {
    auto handset = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server(celestron_responder(handset));
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(server.port()));
        alpacacore::test::SlotBodyTracker slews;
        alpacacore::test::SlotBodyTracker pulses;
        alpacacore::vendor::celestron::set_slew_spawn_for_testing(*driver, slews.spawn());
        alpacacore::vendor::celestron::set_pulse_spawn_for_testing(*driver, pulses.spawn());
        // The slew and pulse bodies really are live at destruction (#1859).
        require_slew_and_pulse_bodies(*driver, *handset, slews, pulses);
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();
        // The destructor joined every body it started.
        CHECK(slews.running() == 0);
        CHECK(pulses.running() == 0);
    }
}

#endif  // !_WIN32
