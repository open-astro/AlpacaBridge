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
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

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

void telescope_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_declination()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    // The newly-fixed thread paths: async slew and pulse guiding issued
    // while the lifecycle threads disconnect underneath them.
    guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
    guard([&] { scope.pulse_guide(0, 50); });
    guard([&] { scope.abort_slew(); });
}

}  // namespace

TEST_CASE("SynScan telescope - concurrent connect/disconnect/slew/pulse stress", "[synscan][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
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
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::NotImplemented,
        alpacacore::AlpacaError::DriverException};
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
    alpacacore::test::FakeMountServer server;
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

#endif  // !_WIN32
