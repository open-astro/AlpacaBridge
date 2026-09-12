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
// tasks, the RightAscensionRate live-rate-verify task from open-astro #248,
// and the sub-floor duty-cycle worker that set_tracking toggles) all spawn
// and get raced by disconnect/destruction exactly as they do against the
// Wave 100i. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <atomic>
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
// bug in AGENTS.md), rate_verify_thread_ (open-astro #248) and the
// duty_thread_ that set_tracking starts and stops.
//
// The rate-verify task only spawns on an in-place rate change: tracking must
// be on (otherwise the setter stores the value and returns), the new rate
// must differ from the stored one (idempotent rewrites return early), and
// both effective rates must sit above the slow-mode floor in the same
// direction. A quarter-sidereal offset keeps the axis continuous, and the
// toggle alternates it with 0.0 so every call is a real change. It also has
// to come BEFORE the slew/pulse/MoveAxis calls: while any of those owns the
// axes the setter only stores the rate for the restore path.
std::atomic<int> g_rate_toggle{0};

void skywatcher_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_declination()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });

    guard([&] { scope.set_tracking(true); });
    const double ra_rate = (g_rate_toggle.fetch_add(1) % 2 == 0) ? 0.25 : 0.0;
    // in-place change spawns rate_verify_thread_ (#248)
    guard([&] { scope.set_right_ascension_rate(ra_rate); });

    guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
    guard([&] { scope.pulse_guide(0, 50); });

    // Alternating-axis MoveAxis start/stop pairs close together — CCDciel
    // issues these ~44 ms apart on button release, which is what let a
    // second axis's stop supersede the first axis's still-ramping stop task.
    guard([&] { scope.move_axis(0, 1.0); });
    guard([&] { scope.move_axis(1, 1.0); });
    guard([&] { scope.move_axis(0, 0.0); });
    guard([&] { scope.move_axis(1, 0.0); });

    guard([&] { scope.set_tracking(false); });
    guard([&] { scope.abort_slew(); });
}

}  // namespace

TEST_CASE("SkyWatcher telescope - concurrent connect/disconnect/slew/pulse/moveaxis stress",
          "[skywatcher][telescope][stress]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // A ramped stop keeps the per-axis stop tasks polling for a while, so the
    // racing disconnects land inside the #212 poll window instead of after a
    // stop that the fake reported complete on its first inquire_status.
    mount.set_stop_ramp_ms(200);
    auto driver = make_driver(mount);

    // Prove the fake can actually be connected to before the storm. Every
    // exception inside the storm is swallowed by design, so without this the
    // whole scenario would pass while never once exercising a connected
    // driver (the PR #3 lesson from the SynScan stress test).
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    // open-astro#326: one guard per call. This callback makes fifteen calls,
    // so before this a storm racing a disconnect exercised get_tracking() and
    // skipped the MoveAxis pairs and rate changes the registration exists for.
    //
    // The set REPLACES the default {NotConnected}. Connected over the UDP
    // simulator, so the driver legitimately reports InvalidValue and
    // InvalidOperation from motion racing motion, the shared NotImplemented
    // code, and DriverException where the simulator's canned replies cannot
    // answer. A real defect shows up as a code outside this set, and
    // guard.report() names every distinct one it saw.
    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::NotImplemented,
        alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { skywatcher_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
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
    mount.set_stop_ramp_ms(200);  // keep both stop tasks alive across the reset

    for (int i = 0; i < 10; ++i) {
        auto driver = make_driver(mount);
        // A hard assertion, not a discarded result: the worker threads this
        // scenario destroys mid-flight only exist on a connected driver, so a
        // fake that cannot be connected to would silently reduce this to
        // destroying an idle object.
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        // Tracking on, then a non-zero rate on a fresh driver (stored rate
        // 0.0) while the axes are still free: the in-place path spawns
        // rate_verify_thread_. After the slew below the setter would only
        // store the rate.
        try {
            driver->set_tracking(true);
        } catch (const std::exception&) {
        }
        try {
            driver->set_right_ascension_rate(0.25);
        } catch (const std::exception&) {
        }
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
        // MoveAxis(axis, 0) on a moving axis is what spawns stop_task_thread_[axis].
        try {
            driver->move_axis(0, 0.0);
        } catch (const std::exception&) {
        }
        try {
            driver->move_axis(1, 0.0);
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
