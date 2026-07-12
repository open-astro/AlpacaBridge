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
// through a FakeMountServer: the protocol wrapper treats a successful TCP
// connect as mount-connected and every post-connect query tolerates failure,
// so a canned "0#" responder is enough to spawn the driver's worker threads
// on a hardware-free host. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

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
    info.response_timeout_ms = 50;  // dumb canned replies leave some reads to time out — keep that cheap
    return info;
}

// NexStar responses are mostly fixed-length payloads drained to a trailing
// '#'. A 2-byte "0#" reply leaves every longer fixed-length read waiting out
// its timeout (the connect sequence probes firmware, model and the whole AUX
// bus), which made these tests minutes-slow. A position-pair sized reply with
// the trailing '#' terminates every read immediately; payload parse failures
// remain tolerated by the driver.
alpacacore::test::FakeMountServer::Responder celestron_responder() {
    return [](const std::string&) { return std::string("00000000,00000000#"); };
}

void telescope_operate(AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    static_cast<void>(scope.get_tracking());
    static_cast<void>(scope.get_right_ascension());
    static_cast<void>(scope.get_declination());
    static_cast<void>(scope.get_slewing());
    // The newly-fixed thread paths: async slew and pulse guiding issued
    // while the lifecycle threads disconnect underneath them.
    scope.slew_to_coordinates_async(5.0, 20.0);
    scope.pulse_guide(0, 50);
    scope.abort_slew();
}

}  // namespace

TEST_CASE("Celestron telescope - concurrent connect/disconnect/slew/pulse stress", "[celestron][telescope][stress]") {
    alpacacore::test::FakeMountServer server(celestron_responder());
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(server.port()));

    alpacacore::test::run_lifecycle_stress(*driver, telescope_operate);

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Celestron telescope - destruction races an in-flight connect", "[celestron][telescope][stress]") {
    alpacacore::test::FakeMountServer server(celestron_responder());
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(port)); },
        // 25 iterations, not the default 100: with the dumb canned replies a
        // fake-connected mount's connect sequence rides several read
        // timeouts, and each destruction joins the in-flight connect.
        25);
}

TEST_CASE("Celestron telescope - destruction mid-operation (slew/pulse threads live)",
          "[celestron][telescope][stress]") {
    alpacacore::test::FakeMountServer server(celestron_responder());
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, celestron_endpoint(server.port()));
        static_cast<void>(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
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
