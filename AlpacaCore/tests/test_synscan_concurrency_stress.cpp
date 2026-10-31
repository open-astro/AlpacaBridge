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

// Connect/disconnect/operate concurrency stress for the SynScan hand-controller
// telescope (audit follow-up, 3.0.1). The audited detached-thread fixes on
// this driver (async slew + pulse-guide threads racing set_connected(false)
// and destruction) only run on a CONNECTED driver, so these tests connect
// through a FakeMountServer: the protocol wrapper treats a successful TCP
// connect as mount-connected and every post-connect query tolerates failure,
// so a canned "0#" responder is enough to spawn the driver's worker threads
// on a hardware-free host. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

using alpacacore::AlpacaDriver;

namespace {

alpacacore::vendor::synscan::ConnectionInfo synscan_endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 50;  // dumb canned replies leave some reads to time out — keep that cheap
    return info;
}

// default_responder() answers "0#" to `t`, i.e. Tracking false, and the slew
// initiators refuse that with InvalidOperation before the dispatch thread
// starts (open-astro#775). Answer tracking on so the slew threads really run.
alpacacore::test::FakeMountServer::Responder tracking_on_responder() {
    auto base = alpacacore::test::FakeMountServer::default_responder();
    return [base](const std::string& chunk) {
        if (!chunk.empty() && chunk[0] == 't') {
            return std::string("\x01#");
        }
        return base(chunk);
    };
}

// The handset of the lost-link storms: tracking on, and counting position polls (`e`) so a case can tell a live
// status poll from a dead one. `mute` makes it answer nothing at all, the way a hung handset does.
struct MutableHandset {
    std::atomic<bool> mute{false};
    std::atomic<int> position_polls{0};
};

alpacacore::test::FakeMountServer::Responder muteable_responder(std::shared_ptr<MutableHandset> handset) {
    auto base = tracking_on_responder();
    return [base, handset](const std::string& chunk) {
        if (handset->mute.load()) {
            return std::string();
        }
        if (!chunk.empty() && chunk[0] == 'e') {
            handset->position_polls.fetch_add(1);
        }
        return base(chunk);
    };
}

// The task clock is fake so a few seconds of real time can carry the 30 s link-staleness bound; the driver's status
// poll and every task it parks on the clock only move when a case advances it.
std::unique_ptr<alpacacore::TelescopeDriver> synscan_with_clock(int port, alpacacore::test::FakeTaskClock& clock) {
    return alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, synscan_endpoint(port), alpacacore::vendor::synscan::SynScanVersion::V4, -33.856784, 151.215297, 42.0, false,
        alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
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

void lost_link_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { static_cast<void>(scope.get_connected()); });
    guard([&] { static_cast<void>(scope.get_link_fault()); });
    guard([&] { static_cast<void>(scope.get_device_state()); });
    telescope_operate(guard, d);
}

}  // namespace

TEST_CASE("SynScan telescope - concurrent connect/disconnect/slew/pulse stress", "[synscan][telescope][stress]") {
    alpacacore::test::FakeMountServer server(tracking_on_responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, synscan_endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);

    // Prove the fake can actually be connected to before the storm. Every
    // exception inside the storm is swallowed by design, so without this the
    // whole scenario would pass while never once exercising a connected
    // driver - which is exactly what happened when the echo-gated connect
    // landed against a fake that did not answer the echo (PR #3 review).
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    // open-astro#326: one guard per call, replacing no isolation at all --
    // run_lifecycle_stress wraps the WHOLE callback in one try/catch, so a
    // storm racing a disconnect exercised get_tracking() and skipped the six
    // calls below it, including the async slew and pulse-guide paths this
    // registration exists to storm.
    //
    // The set REPLACES the default {NotConnected}. This registration runs
    // CONNECTED over FakeMountServer, so a live driver's operate callback
    // legitimately throws more than that: InvalidValue and InvalidOperation
    // from a slew or pulse racing a park or another motion, and the shared
    // NotImplemented code where the canned fake cannot answer.
    //
    // DriverException is here for the FAKE, not the driver: FakeMountServer
    // answers with deliberately dumb canned replies (this file's own
    // destruction case notes the connect sequence "rides several read
    // timeouts" because of them), so the driver correctly reports "Invalid
    // status response from mount" and the like. Those were invisible while the
    // callback aborted at the first throw; the guard counts them, so they have
    // to be named. A real driver defect would show up as a code outside this
    // set, and guard.report() names every distinct one it saw.
    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected,     alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::ValueNotSet,
        alpacacore::AlpacaError::NotImplemented,   alpacacore::AlpacaError::DriverException};
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

TEST_CASE("SynScan telescope - destruction races an in-flight connect", "[synscan][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() {
            return alpacacore::vendor::synscan::create_synscan_telescope(
                0, synscan_endpoint(port), alpacacore::vendor::synscan::SynScanVersion::V4);
        },
        // 25 iterations, not the default 100: with the dumb canned replies a
        // fake-connected mount's connect sequence rides several read
        // timeouts, and each destruction joins the in-flight connect.
        25);
}

TEST_CASE("SynScan telescope - destruction mid-operation (slew/pulse threads live)", "[synscan][telescope][stress]") {
    alpacacore::test::FakeMountServer server(tracking_on_responder());
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, synscan_endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
        // A hard assertion, not a discarded result: the slew/pulse worker
        // threads this scenario destroys mid-flight only exist on a
        // connected driver, so a fake that cannot be connected to would
        // silently reduce this to destroying an idle object.
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {
        }
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();
    }
}

TEST_CASE("SynScan telescope - handset muted and severed mid-storm, then relinked", "[synscan][telescope][stress]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeTaskClock clock;
    auto handset = std::make_shared<MutableHandset>();
    alpacacore::test::FakeMountServer server(muteable_responder(handset));
    REQUIRE(server.ok());
    auto driver = synscan_with_clock(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 5s));

    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected,     alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::ValueNotSet,
        alpacacore::AlpacaError::NotImplemented,   alpacacore::AlpacaError::DriverException};

    // Mute (and on alternate rounds sever) the handset while the lifecycle and operate threads run, and move the
    // task clock past the staleness bound so the lifecycle-churn phase races the poll against connect/disconnect. Each
    // tick of the clock needs the poll (50 ms response timeout) to fail in real time.
    std::atomic<bool> stop{false};
    std::thread chaos([&] {
        for (int round = 0; !stop.load(); ++round) {
            handset->mute = true;
            if ((round % 2) != 0) {
                static_cast<void>(server.drop_connections(200ms));
            }
            for (int tick = 0; tick < 4 && !stop.load(); ++tick) {
                std::this_thread::sleep_for(80ms);
                clock.advance(1s);
            }
            clock.advance(alpacacore::util::kLinkStalenessBound + 1s);
            std::this_thread::sleep_for(80ms);
            handset->mute = false;
            std::this_thread::sleep_for(60ms);
        }
    });
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { lost_link_operate(guard, d); });
    stop = true;
    chaos.join();

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);

    // Lost-link phase: no lifecycle threads, so nothing resets the link health or stops the poll. The chaos thread
    // mutes and severs the handset, advances the clock until Connected drops (the #1257 latch, then the lost-link
    // declaration), unmutes and relinks, while getters and slews run on the operate threads.
    std::atomic<bool> phase_stop{false};
    std::atomic<int> lost_rounds{0};
    std::atomic<int> relinked_rounds{0};
    std::thread lost_chaos([&] {
        while (!phase_stop.load()) {
            handset->mute = true;
            static_cast<void>(server.drop_connections(200ms));
            const auto lost_deadline = std::chrono::steady_clock::now() + 2s;
            while (driver->get_connected() && !phase_stop.load() && std::chrono::steady_clock::now() < lost_deadline) {
                clock.advance(5s);
                std::this_thread::sleep_for(30ms);
            }
            if (!driver->get_connected()) {
                ++lost_rounds;
            }
            handset->mute = false;
            guard([&] { driver->set_connected(true); });
            if (driver->get_connected()) {
                ++relinked_rounds;
            }
            std::this_thread::sleep_for(60ms);
        }
    });
    alpacacore::test::StressOptions lost_opt;
    lost_opt.lifecycle_threads = 0;
    lost_opt.duration = 3s;
    alpacacore::test::run_lifecycle_stress(
        *driver, [&guard](AlpacaDriver& d) { lost_link_operate(guard, d); }, lost_opt);
    phase_stop = true;
    lost_chaos.join();
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(lost_rounds.load() > 0);
    CHECK(relinked_rounds.load() > 0);

    // Unmuted, a Connected=true relinks whatever state the storm left (a lost link included).
    handset->mute = false;
    CHECK(alpacacore::test::settle_connected(*driver, false, 5s));
    CHECK(alpacacore::test::settle_connected(*driver, true, 5s));
    CHECK(driver->get_connected());

    // A severed, silent handset reaches the lost-link state within a bounded time: three failed polls latch the
    // fault, and a fault standing past the staleness bound drops Connected.
    handset->mute = true;
    static_cast<void>(server.drop_connections(200ms));
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (driver->get_connected() && std::chrono::steady_clock::now() < deadline) {
        clock.advance(5s);
        std::this_thread::sleep_for(30ms);
    }
    REQUIRE_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_link_fault().empty());
    try {
        static_cast<void>(driver->get_right_ascension());
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }

    // Decision 0009 point 4: the client's Connected=true relinks, and the relinked driver polls again.
    handset->mute = false;
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 5s));
    CHECK(driver->get_connected());
    const int before = handset->position_polls.load();
    const auto poll_deadline = std::chrono::steady_clock::now() + 5s;
    while (handset->position_polls.load() == before && std::chrono::steady_clock::now() < poll_deadline) {
        clock.advance(1s);
        std::this_thread::sleep_for(20ms);
    }
    CHECK(handset->position_polls.load() > before);
    CHECK(alpacacore::test::settle_connected(*driver, false, 5s));
}

// set_connected() calls are serialised on transition_mutex_, so a concurrent set_connected(false) lands before the
// connect starts or after it has finished. Whichever way the race goes the driver must end consistent: connected
// means a live status poll, disconnected means none.
TEST_CASE("SynScan telescope - connect racing disconnect leaves the status poll matching Connected",
          "[synscan][telescope][stress]") {
    using namespace std::chrono_literals;
    alpacacore::test::FakeTaskClock clock;
    auto handset = std::make_shared<MutableHandset>();
    alpacacore::test::FakeMountServer server(muteable_responder(handset));
    REQUIRE(server.ok());
    auto driver = synscan_with_clock(server.port(), clock);

    int connected_rounds = 0;
    for (int i = 0; i < 6; ++i) {
        std::thread connecting([&] {
            try {
                driver->set_connected(true);
            } catch (const std::exception&) {
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60 * i));
        try {
            driver->set_connected(false);
        } catch (const std::exception&) {
        }
        connecting.join();

        const int before = handset->position_polls.load();
        if (driver->get_connected()) {
            ++connected_rounds;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (handset->position_polls.load() == before && std::chrono::steady_clock::now() < deadline) {
                clock.advance(1s);
                std::this_thread::sleep_for(20ms);
            }
            CHECK(handset->position_polls.load() > before);
            CHECK(alpacacore::test::settle_connected(*driver, false, 5s));
        } else {
            clock.advance(3s);
            std::this_thread::sleep_for(150ms);
            CHECK(handset->position_polls.load() == before);
        }
    }
    INFO("rounds that ended connected: " << connected_rounds);
    // A final round that certainly ends connected, so the poll check cannot pass vacuously when every racing
    // round above ended disconnected.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, 5s));
    const int final_before = handset->position_polls.load();
    const auto final_deadline = std::chrono::steady_clock::now() + 5s;
    while (handset->position_polls.load() == final_before && std::chrono::steady_clock::now() < final_deadline) {
        clock.advance(1s);
        std::this_thread::sleep_for(20ms);
    }
    CHECK(handset->position_polls.load() > final_before);
    CHECK(alpacacore::test::settle_connected(*driver, false, 5s));
}

#endif  // !_WIN32
