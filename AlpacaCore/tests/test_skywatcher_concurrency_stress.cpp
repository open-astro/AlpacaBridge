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

// Connect/disconnect/operate concurrency stress for the SkyWatcher motor
// controller telescope (issue #271). The driver connects through its REAL
// protocol wrapper and UDP transport to FakeSkyWatcherMount, a loopback
// motor-controller simulator with a continuous axis model, so the driver's
// worker threads (async slew, pulse-guide, the two per-axis MoveAxis stop
// tasks, and the RightAscensionRate live-rate-verify task from open-astro
// #248) all spawn and get raced by disconnect/destruction exactly as they do
// against the Wave 100i. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::AlpacaDriver;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

sw::ConnectionInfo endpoint_for_port(int port) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = port;
    info.response_timeout_ms = 250;
    return info;
}

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) { return endpoint_for_port(mount.port()); }

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(const FakeSkyWatcherMount& mount) {
    return sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0);
}

// Hammers every worker thread the driver's disconnect/destructor path must
// join: slew_task_thread_, pulse_task_thread_, both per-axis
// stop_task_thread_[axis] (MoveAxis stops issued close together — the exact
// shape of the 2026-09-06 "superseded MoveAxis stop task strands Slewing"
// bug in AGENTS.md), and rate_verify_thread_ (open-astro #248).
void skywatcher_operate(AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    static_cast<void>(scope.get_tracking());
    static_cast<void>(scope.get_right_ascension());
    static_cast<void>(scope.get_declination());
    static_cast<void>(scope.get_slewing());

    scope.slew_to_coordinates_async(5.0, 20.0);
    scope.pulse_guide(0, 50);

    // Alternating-axis MoveAxis start/stop pairs close together — CCDciel
    // issues these ~44 ms apart on button release, which is what let a
    // second axis's stop supersede the first axis's still-ramping stop task.
    scope.move_axis(0, 1.0);
    scope.move_axis(1, 1.0);
    scope.move_axis(0, 0.0);
    scope.move_axis(1, 0.0);

    scope.set_right_ascension_rate(0.0);  // spawns rate_verify_thread_ (#248)
    scope.set_tracking(true);
    scope.set_tracking(false);
    scope.abort_slew();
}

}  // namespace

TEST_CASE("SkyWatcher telescope - concurrent connect/disconnect/slew/pulse/moveaxis stress",
          "[skywatcher][telescope][stress]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = make_driver(mount);

    // Prove the fake can actually be connected to before the storm. Every
    // exception inside the storm is swallowed by design, so without this the
    // whole scenario would pass while never once exercising a connected
    // driver (the PR #3 lesson from the SynScan stress test).
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    alpacacore::test::run_lifecycle_stress(*driver, skywatcher_operate);

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);
}

TEST_CASE("SkyWatcher telescope - destruction races an in-flight connect", "[skywatcher][telescope][stress]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    const int port = mount.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return sw::create_skywatcher_telescope(0, endpoint_for_port(port), 39.7392, -104.9903, 1609.0); },
        // 25 iterations, not the default 100: each destroyed connect can ride
        // a 250 ms UDP response timeout (up to 3 retransmits per query), same
        // reasoning as the SynScan stress test.
        25);
}

TEST_CASE("SkyWatcher telescope - destruction mid-operation (slew/pulse/stop/rate-verify threads live)",
          "[skywatcher][telescope][stress]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = make_driver(mount);
        // A hard assertion, not a discarded result: the worker threads this
        // scenario destroys mid-flight only exist on a connected driver, so a
        // fake that cannot be connected to would silently reduce this to
        // destroying an idle object.
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {
        }
        try {
            driver->move_axis(0, 1.0);
        } catch (const std::exception&) {
        }
        try {
            driver->move_axis(1, 1.0);
        } catch (const std::exception&) {
        }
        try {
            driver->set_right_ascension_rate(0.0);
        } catch (const std::exception&) {
        }
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();  // ~Driver must join every in-flight worker thread
    }
}

TEST_CASE("SkyWatcher telescope - racing disconnect is never dropped", "[skywatcher][telescope][stress]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = make_driver(mount);

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
}

#endif  // !_WIN32
