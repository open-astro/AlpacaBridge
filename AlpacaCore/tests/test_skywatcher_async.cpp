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

// Hardware-free coverage of the SkyWatcher driver's async state machines
// (issue #213): the driver connects through its REAL protocol wrapper and UDP
// transport to FakeSkyWatcherMount, a loopback motor-controller simulator
// with a continuous axis model, so slew dispatch + landing refinement, the
// Park/FindHome tasks, pulse-guide timers, and MoveAxis stop tasks all run
// end-to-end exactly as they do against the Wave 100i.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0);
    driver->set_connected(true);
    return driver;
}

// Poll a predicate with a deadline, advancing in small steps.
bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

}  // namespace

TEST_CASE("SkyWatcher async - connect and live reads through the fake mount", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE(driver->get_connected());
    // At count home the pointing model reports the pole.
    REQUIRE(std::abs(driver->get_declination() - 90.0) < 0.1);
    REQUIRE_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - async slew lifecycle lands on target and restores tracking", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 40.0);
    REQUIRE(driver->get_slewing());

    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    // Landed within the refinement deadband (plus a small read margin).
    REQUIRE(std::abs(driver->get_declination() - 40.0) < 0.05);
    double ra_err_arcsec = std::abs(driver->get_right_ascension() - target_ra) * 3600.0 * 15.0;
    REQUIRE(ra_err_arcsec < 30.0);
    REQUIRE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - Park completes and Unpark cancels an in-flight park", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);

    driver->park();
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());

    // Unpark DURING a park must win the race and leave the mount unparked.
    // Wait until the park task has actually started the slew so the race
    // window is genuinely exercised, not skipped by a fast dispatch.
    mount.jump_axis_degrees(1, 25.0);
    driver->park();
    REQUIRE(wait_until([&] { return mount.axis_running(1) || mount.axis_running(2); }, 5000));
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - FindHome runs AutoHome against the index sensors", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // Physical home sensor sits 2 degrees away from where the counts claim
    // home is — AutoHome must find it and re-anchor the count frame.
    mount.set_home_index_degrees(1, 2.0);
    mount.set_home_index_degrees(2, 2.0);
    auto driver = connected_driver(mount);

    driver->find_home();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return driver->get_at_home(); }, 60000));
    REQUIRE_FALSE(driver->get_slewing());
    // The axes physically sit at the sensor position, and the counts were
    // re-stamped so the driver now reads it as home (the pole).
    REQUIRE(std::abs(mount.physical_degrees(1) - 2.0) < 0.2);
    REQUIRE(std::abs(driver->get_declination() - 90.0) < 0.2);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - pulse guide north physically moves Dec and ends cleanly", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch (a2 > 0)

    double dec_before = mount.axis_degrees(2);
    driver->pulse_guide(0, 1500);  // North, 1.5 s at the 0.5x default rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    // ~0.5x sidereal x 1.5 s ≈ 11 arcsec of physical axis motion; the
    // east-branch sign rule makes +Dec NEGATIVE axis motion.
    double moved_arcsec = (mount.axis_degrees(2) - dec_before) * 3600.0;
    REQUIRE(moved_arcsec < -6.0);
    REQUIRE(moved_arcsec > -20.0);
    // Dec axis stopped again after the pulse.
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 3000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - MoveAxis stop task clears Slewing and restores tracking", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    driver->move_axis(0, 2.0);
    REQUIRE(driver->get_slewing());
    driver->move_axis(0, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    REQUIRE(wait_until([&] { return driver->get_tracking(); }, 5000));

    // MoveAxis(0) on an already-stationary axis stays a no-op.
    driver->move_axis(1, 0.0);
    REQUIRE_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - AbortSlew cancels the slew task without a refinement re-goto", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0);
    REQUIRE(driver->get_slewing());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    driver->abort_slew();
    REQUIRE_FALSE(driver->get_slewing());

    // The cancelled slew task must not fire a refinement goto afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(mount.axis_running(1));
    REQUIRE_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - reads stay responsive while an axis stop is ramping", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);  // real Wave axes take ~1 s to decelerate
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    driver->move_axis(0, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    driver->move_axis(0, 0.0);  // async stop: the mount now ramps down for 800 ms

    // Issue #212: the stop-wait must RELEASE the driver mutex between polls,
    // so concurrent position reads answer promptly while the axis ramps.
    int slow_reads = 0;
    for (int i = 0; i < 6; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        static_cast<void>(driver->get_right_ascension());
        static_cast<void>(driver->get_slewing());
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        if (ms.count() > 250) {
            ++slow_reads;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(slow_reads == 0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    REQUIRE(wait_until([&] { return driver->get_tracking(); }, 5000));
    driver->set_connected(false);
}

#endif  // _WIN32
