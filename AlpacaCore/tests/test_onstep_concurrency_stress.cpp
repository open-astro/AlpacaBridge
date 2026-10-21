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

// Connect/disconnect/operate concurrency stress for the OnStep telescope
// driver, mirroring the SynScan/iOptron [stress] coverage (audit 3.0.1
// follow-up). OnStep is USB-serial only for end users (see
// .github/instructions/onstep.instructions.md), but
// the protocol wrapper's ConnectionType::Network branch exists purely as a
// test seam so this suite can drive the driver into the CONNECTED state
// through a FakeMountServer without real hardware — a canned "0#" responder
// answers every '#'-framed LX200 command, which is enough to exercise the
// driver's connect/disconnect/operate/destroy paths on a hardware-free host.
// Operations racing a disconnect are EXPECTED to throw; what must never
// happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_pty_write.h"

using alpacacore::AlpacaDriver;

namespace {

alpacacore::vendor::onstep::ConnectionInfo onstep_endpoint(int port) {
    alpacacore::vendor::onstep::ConnectionInfo info;
    info.type = alpacacore::vendor::onstep::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 50;  // dumb canned replies leave some reads to time out — keep that cheap
    return info;
}

void telescope_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { scope.set_target_right_ascension(5.0); });
    guard([&] { scope.set_target_declination(20.0); });
    guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_target_declination()); });
    guard([&] { scope.slew_to_target_async(); });
    guard([&] { scope.sync_to_target(); });
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_declination()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
    guard([&] { scope.pulse_guide(0, 50); });
    guard([&] { scope.abort_slew(); });
}

}  // namespace

TEST_CASE("OnStep telescope - concurrent connect/disconnect/slew/pulse stress", "[onstep][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, onstep_endpoint(server.port()));

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

TEST_CASE("OnStep telescope - destruction races an in-flight connect", "[onstep][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::onstep::create_onstep_telescope(0, onstep_endpoint(port)); },
        // 25 iterations, not the default 100: with the dumb canned replies a
        // fake-connected mount's connect sequence rides several read
        // timeouts, and each destruction joins the in-flight connect.
        25);
}

TEST_CASE("OnStep telescope - destruction mid-operation (slew/pulse threads live)", "[onstep][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, onstep_endpoint(server.port()));
        static_cast<void>(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Expected: a fake-connected mount can reject/race this call.
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Expected: see above.
        }
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();
    }
}

// open-astro#1289: link_alive() runs with no wrapper mutex_ held, from
// get_link_fault() and get_connected(), while connect and
// disconnect change the recorded node and an unplug or replug changes what
// the port path points at. A storm of all three over a serial path that is
// swapped between two ptys and removed, so the generation counter,
// link_id_mutex_, loss_mutex_ and the try_lock close all run under TSan. A
// serial connect only opens the port, so the ptys need no replies.
TEST_CASE("OnStep wrapper - link_alive races connect and disconnect during an adapter swap",
          "[onstep][telescope][stress]") {
    using alpacacore::vendor::onstep::ConnectionInfo;
    using alpacacore::vendor::onstep::ConnectionType;
    using alpacacore::vendor::onstep::OnStepProtocolWrapper;
    auto& protocol = OnStepProtocolWrapper::instance();
    const auto reset_wrapper = [&protocol]() {
        protocol.disconnect();
        static_cast<void>(protocol.consume_link_lost_at());
        static_cast<void>(protocol.consume_last_alive_before_loss());
    };
    reset_wrapper();

    alpacacore::test::PtyPair first("OnStep stress adapter A");
    alpacacore::test::PtyPair second("OnStep stress adapter B");
    const std::filesystem::path link = std::filesystem::temp_directory_path() /
                                       ("alpacacore-onstep1289-stress-" + std::to_string(::getpid()) + "-" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_symlink(first.slave_path(), link);

    ConnectionInfo info;
    info.type = ConnectionType::Serial;
    info.port_path = link.string();
    info.response_timeout_ms = 200;

    // None of these calls should throw: a connect whose path is missing at
    // that instant returns false. The default expected set ({NotConnected})
    // stays, so any other throw, e.g. the "one mount per bridge" refusal if a
    // disconnect ever left the wrapper connected, is counted as a failure.
    alpacacore::test::StressCallGuard guard;
    std::atomic<bool> stop{false};
    std::thread checker;
    std::thread swapper;
    // Joins both workers on every way out, so a throw between the two thread
    // starts cannot leave one joinable at destruction.
    struct JoinBoth {
        std::atomic<bool>& stop;
        std::thread& a;
        std::thread& b;
        ~JoinBoth() {
            stop.store(true);
            if (a.joinable()) a.join();
            if (b.joinable()) b.join();
        }
    } join_both{stop, checker, swapper};

    checker = std::thread([&]() {
        while (!stop.load()) {
            guard([&] { static_cast<void>(protocol.link_alive()); });
            guard([&] { static_cast<void>(protocol.link_lost_reason()); });
        }
    });
    swapper = std::thread([&]() {
        int step = 0;
        while (!stop.load()) {
            std::error_code ec;
            std::filesystem::remove(link, ec);
            if (step % 3 != 2) {  // every third step leaves the path missing for a while
                std::filesystem::create_symlink((step % 2 == 0 ? second : first).slave_path(), link, ec);
            }
            ++step;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Counted so the case fails if the storm stops reaching the paths it is
    // for: connects that open a link, and losses recorded while one is up.
    int connects = 0;
    int losses = 0;
    for (int i = 0; i < 2000; ++i) {
        guard([&] {
            if (protocol.connect(info)) ++connects;
        });
        guard([&] { static_cast<void>(protocol.link_alive()); });
        guard([&] {
            if (!protocol.link_lost_reason().empty()) ++losses;
        });
        guard([&] { protocol.disconnect(); });
    }
    stop.store(true);
    checker.join();
    swapper.join();

    reset_wrapper();
    std::error_code ec;
    std::filesystem::remove(link, ec);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    INFO("connects " << connects << ", losses " << losses);
    CHECK(connects > 0);
    CHECK(losses > 0);
}

#endif  // !_WIN32
