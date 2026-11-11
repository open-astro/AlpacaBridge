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

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "driver_rig.h"
#include "fake_celestron_cgx_mount.h"
#include "getter_stall_probe.h"

namespace {

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(
    const std::shared_ptr<alpacacore::test::FakeCelestronCgxMount>& mount,
    alpacacore::vendor::celestron::CgxSystemNow system_now = {}) {
    mount->set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
    mount->set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
    mount->set_cgx_switches_present(false);
    alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
    info.port_path = mount->slave_path();
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    return alpacacore::vendor::celestron::create_celestron_cgx_telescope(
        0, info, alpacacore::util::default_task_clock(), std::move(system_now));
}

void operate(alpacacore::test::StressCallGuard& guard, alpacacore::AlpacaDriver& base) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(base);
    guard([&] { scope.set_target_right_ascension(6.0); });
    guard([&] { scope.set_target_declination(30.0); });
    guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_target_declination()); });
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_tracking_rate()); });
    guard([&] { scope.set_tracking_rate(1); });
    guard([&] { static_cast<void>(scope.get_at_home()); });
    guard([&] { static_cast<void>(scope.get_at_park()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    guard([&] { scope.find_home(); });
    guard([&] { scope.slew_to_coordinates_async(6.0, 30.0); });
    guard([&] { scope.pulse_guide(0, 100); });
    guard([&] { static_cast<void>(scope.get_is_pulse_guiding()); });
    guard([&] { scope.abort_slew(); });
    guard([&] { scope.park(); });
    guard([&] { scope.unpark(); });
    guard([&] { scope.sync_to_coordinates(6.0, 30.0); });
    guard([&] { scope.set_tracking(true); });
    guard([&] { scope.set_tracking(false); });
}

}  // namespace

TEST_CASE("Celestron CGX USB telescope - connection and motion lifecycle stress", "[celestron][telescope][stress]") {
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);

    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::InvalidWhileParked,
        alpacacore::AlpacaError::NotImplemented};
    auto driver = make_driver(mount);
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](alpacacore::AlpacaDriver& d) { operate(guard, d); });
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

// The state poll (#1940) runs on the real clock while connect, disconnect and motion race the getters it feeds. The
// coordinate getters may refuse with DriverException (decision 0011: no reference before FindHome, a stop still
// pending, motion not verified), so they have a guard of their own; the motion calls keep the strict set above. No
// case here loses the link (the fake never stops answering), so NotConnected comes from the disconnects alone.
TEST_CASE("Celestron CGX USB telescope - getters read the polled frame through connect and motion storms",
          "[celestron][telescope][stress]") {
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);

    alpacacore::test::StressCallGuard motion{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::InvalidWhileParked,
        alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::StressCallGuard served{alpacacore::AlpacaError::NotConnected,
                                             alpacacore::AlpacaError::DriverException};
    auto driver = make_driver(mount);
    alpacacore::test::run_lifecycle_stress(*driver, [&](alpacacore::AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        motion([&] { scope.find_home(); });
        served([&] { static_cast<void>(scope.get_right_ascension()); });
        served([&] { static_cast<void>(scope.get_declination()); });
        served([&] { static_cast<void>(scope.get_altitude()); });
        served([&] { static_cast<void>(scope.get_azimuth()); });
        served([&] { static_cast<void>(scope.get_side_of_pier()); });
        served([&] { static_cast<void>(scope.get_device_state()); });
        motion([&] { scope.sync_to_coordinates(6.0, 30.0); });
        motion([&] { scope.abort_slew(); });
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(motion.report());
    CHECK(motion.unexpected_count() == 0);
    CHECK(motion.total_calls() > 0);
    INFO(served.report());
    CHECK(served.unexpected_count() == 0);
    CHECK(served.total_calls() > 0);
}

// #2035: every control-path write to the state frame drops the poll in flight and keeps measured_at (decision 0010
// rules 2 and 3), so a writer in a tight loop would starve the poll and age the frame into a false link loss. The
// pointing clock runs 5000x fast, so every arrival misses by a few arcseconds and the refinement loop runs in full (a
// read per correction, kMaxSlewCorrections + 1 per slew) for 4 s on the real poll. The fake ends a slew in about 2 ms,
// so a 100 ms pause after each one keeps the cadence at ten slews a second: far faster than a real CGX slew (seconds),
// but not the near-continuous writing that the fake alone would give, which can drop several polls in a row on a slow
// host. The served frame's age, from DeviceState's TimeStamp, must stay within a few poll periods and the link must
// not be lost. Check 26 in scripts/check_docs_drift.py pins the writers themselves.
TEST_CASE("Celestron CGX USB telescope - the state frame stays fresh through the arrival-refinement loop",
          "[celestron][telescope][stress]") {
    using namespace std::chrono_literals;
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);
    const auto start = std::chrono::system_clock::now();
    auto driver = make_driver(mount, [start] { return start + (std::chrono::system_clock::now() - start) * 5000; });
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->find_home();
    for (int i = 0; i < 500 && (driver->get_slewing() || !driver->get_at_home()); ++i)
        std::this_thread::sleep_for(10ms);
    REQUIRE(driver->get_at_home());

    // A refinement that runs out of corrections ends the slew with DriverException and may leave tracking off.
    alpacacore::test::StressCallGuard slews{alpacacore::AlpacaError::DriverException,
                                            alpacacore::AlpacaError::InvalidOperation};
    std::atomic<bool> done{false};
    auto max_age = std::chrono::system_clock::duration::zero();
    std::thread worker([&] {
        while (!done.load()) {
            slews([&] { driver->set_tracking(true); });
            slews(
                [&] { driver->slew_to_coordinates_async(std::fmod(driver->get_sidereal_time() + 23.0, 24.0), 30.0); });
            for (int i = 0; i < 500; ++i) {
                bool slewing = false;
                slews([&] { slewing = driver->get_slewing(); });
                if (!slewing) break;
                std::this_thread::sleep_for(2ms);
            }
            std::this_thread::sleep_for(100ms);
        }
    });
    {
        const alpacacore::test::ReleaseAndJoin stop{done, worker};  // stops the slews on every exit, a failure too
        for (const auto end = std::chrono::steady_clock::now() + 4s; std::chrono::steady_clock::now() < end;) {
            const auto stamp = alpacacore::test::time_stamp_of(*driver);
            REQUIRE_FALSE(stamp.empty());  // empty only once disconnected: the link was lost
            max_age = std::max(max_age, std::chrono::system_clock::now() - alpacacore::test::parse_timestamp(stamp));
            std::this_thread::sleep_for(50ms);
        }
    }

    const int corrections = mount->count_command(0x10, 0x17) + mount->count_command(0x11, 0x17);
    INFO("max frame age " << std::chrono::duration_cast<std::chrono::milliseconds>(max_age).count() << " ms, "
                          << corrections << " corrections");
    CHECK(corrections > 0);  // the refinement loop ran
    CHECK(max_age < 2s);     // the poll period is 250 ms; util::kLinkStalenessBound is 30 s
    CHECK(driver->get_connected());
    CHECK_FALSE(alpacacore::vendor::celestron::consume_link_lost_at_for_testing(*driver).has_value());
    INFO(slews.report());
    CHECK(slews.unexpected_count() == 0);
    CHECK(slews.total_calls() > 0);
    driver->set_connected(false);
}

TEST_CASE("Celestron CGX USB telescope - destruction races an in-flight connect", "[celestron][telescope][stress]") {
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);
    alpacacore::test::run_destruction_during_connect_stress([mount] { return make_driver(mount); }, 25);
}

#endif  // !_WIN32
