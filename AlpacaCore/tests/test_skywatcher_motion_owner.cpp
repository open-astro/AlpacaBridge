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
// Motion-owner follow-ups (issue #2467): tests that pin the #2263 motion-claim table for this driver.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
using alpacacore::test::FakeSkyWatcherMount;
using alpacacore::test::FakeTaskClock;

namespace {

constexpr auto kRendezvous = std::chrono::milliseconds(2000);
constexpr std::size_t kPollerWaiter = 1;  // the connected driver's status poll, always parked on the clock
constexpr auto kClockStep = std::chrono::milliseconds(50);

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount, FakeTaskClock& clock) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    auto driver = sw::create_skywatcher_telescope(0, info, 51.477833, -0.0015, 42.0, {}, {}, clock);
    driver->set_connected(true);
    return driver;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// One step of virtual time once a driver thread besides the poller is parked on the clock.
bool step_clock(FakeTaskClock& clock) {
    if (!clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous)) {
        return false;
    }
    clock.advance(kClockStep);
    return clock.wait_for_woken_settled(kRendezvous);
}

bool run_clock_until(FakeTaskClock& clock, const std::function<bool()>& pred, std::chrono::milliseconds budget) {
    for (std::chrono::milliseconds done{0}; done < budget; done += kClockStep) {
        if (pred()) {
            return true;
        }
        if (!step_clock(clock)) {
            return pred();
        }
    }
    return wait_until(pred, 200);
}

struct SinkGuard {
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
};

}  // namespace

// #2467: the per-axis stop-waits that nothing pinned. Each parks on a Dec or RA axis that reads running, and a
// MoveAxis on the OTHER axis lands in the window: it claims one axis, so the wait is not superseded and finishes.
// Tracking=false waits RA, then the running Dec offset.
// Falsified by: skywatcher_telescope_driver.cpp set_tracking(false) dropping its Dec stop_axis_and_wait_locked(): the
// Dec offset is never stopped.
TEST_CASE("SkyWatcher motion owner - an RA MoveAxis during Tracking=false's Dec offset stop-wait does not supersede it",
          "[skywatcher][motion-owner]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    driver->set_declination_rate(10.0);  // a continuous Dec offset: Tracking=false must stop it too
    REQUIRE(mount.axis_running(1));
    REQUIRE(mount.axis_running(2));
    mount.hold_running(1, true);
    mount.hold_running(2, true);
    const int stops = mount.frames_seen('K');

    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread off([&] {
        try {
            driver->set_tracking(false);
        } catch (const alpacacore::AlpacaException&) {
            threw = true;
        }
        done = true;
    });
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // parked in the RA stop-wait
    mount.hold_running(1, false);
    int steps = 0;
    while (mount.frames_seen('K') < stops + 2 && ++steps < 40) {
        REQUIRE(step_clock(clock));
    }
    REQUIRE(mount.frames_seen('K') >= stops + 2);                     // RA's stop, then Dec's
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // parked in the Dec stop-wait
    driver->move_axis(0, 1.0);                                        // RA only
    mount.hold_running(2, false);
    const bool settled = run_clock_until(clock, [&] { return done.load(); }, std::chrono::milliseconds(10000));
    off.join();
    REQUIRE(settled);
    CHECK_FALSE(threw.load());
    CHECK_FALSE(driver->get_tracking());
    CHECK_FALSE(mount.axis_running(2));  // the Dec offset stopped
    CHECK(mount.axis_running(1));        // the RA jog runs on
    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

// #2467: a DeclinationRate write that stops a running Dec offset (to zero, or down to a duty-cycled rate under
// the floor) waits for Dec with the locks released. An RA MoveAxis in that window does not supersede it.
// Falsified by: skywatcher_telescope_driver.cpp apply_dec_rate_offset_locked() dropping the stop_axis_and_wait_locked()
// of either branch: the write returns with Dec still running while the stop is parked.
TEST_CASE("SkyWatcher motion owner - an RA MoveAxis during a Dec offset stop-wait does not supersede it",
          "[skywatcher][motion-owner]") {
    const double dec_rate = GENERATE(0.0, 0.1);  // arcsec/s: off, or under the ~0.26 floor
    CAPTURE(dec_rate);
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    driver->set_declination_rate(10.0);
    REQUIRE(mount.axis_running(2));
    mount.hold_running(2, true);

    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread write([&] {
        try {
            driver->set_declination_rate(dec_rate);
        } catch (const alpacacore::AlpacaException&) {
            threw = true;
        }
        done = true;
    });
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // parked in the Dec stop-wait
    CHECK_FALSE(done.load());                                         // the write waits for the stop
    driver->move_axis(0, 1.0);                                        // RA only
    mount.hold_running(2, false);
    const bool settled = run_clock_until(clock, [&] { return done.load(); }, std::chrono::milliseconds(10000));
    write.join();
    REQUIRE(settled);
    CHECK_FALSE(threw.load());
    CHECK(driver->get_declination_rate() == dec_rate);
    CHECK(mount.axis_running(1));  // the RA jog runs on
    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

// #2467: a RightAscensionRate write that leaves the RA drive under the slow-mode floor stops RA (the duty worker
// then bursts it). A Dec MoveAxis during that RA stop-wait does not supersede it.
// Falsified by: skywatcher_telescope_driver.cpp apply_ra_drive_locked() dropping its stop_axis_and_wait_locked().
TEST_CASE("SkyWatcher motion owner - a Dec MoveAxis during a duty-rate RA stop-wait does not supersede it",
          "[skywatcher][motion-owner]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    REQUIRE(mount.axis_running(1));
    mount.hold_running(1, true);

    std::atomic<bool> done{false};
    std::atomic<bool> threw{false};
    std::thread write([&] {
        try {
            driver->set_right_ascension_rate(0.99);  // ~0.15 arcsec/s: under the floor
        } catch (const alpacacore::AlpacaException&) {
            threw = true;
        }
        done = true;
    });
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // parked in the RA stop-wait
    CHECK_FALSE(done.load());
    driver->move_axis(1, 1.0);  // Dec only
    mount.hold_running(1, false);
    const bool settled = run_clock_until(clock, [&] { return done.load(); }, std::chrono::milliseconds(10000));
    write.join();
    REQUIRE(settled);
    CHECK_FALSE(threw.load());
    CHECK(driver->get_tracking());
    CHECK(driver->get_right_ascension_rate() == 0.99);
    CHECK(mount.axis_running(2));  // the Dec jog runs on
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

// #2467: a pulse dispatch that a newer motion command took the axis from is expected, so it logs at INFO, not WARN.
// Falsified by: skywatcher_telescope_driver.cpp's pulse dispatch catch logging the superseded dispatch at WARN.
TEST_CASE("SkyWatcher motion owner - a superseded pulse dispatch logs INFO, not WARN (#2467)",
          "[skywatcher][motion-owner]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(2, 45.0);
    SinkGuard sink_guard;
    std::atomic<bool> warned{false};
    std::atomic<bool> informed{false};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (message.find("PulseGuide dispatch") == std::string_view::npos) {
                return;
            }
            if (level == alpacacore::logging::LogLevel::Warn) {
                warned = true;
            } else if (level == alpacacore::logging::LogLevel::Info &&
                       message.find("superseded") != std::string_view::npos) {
                informed = true;
            }
        });
    mount.hold_running(2, true);  // Dec reads running: the pulse's dispatch parks in its stop-wait

    driver->pulse_guide(0, 500);
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // dispatch parked in the Dec stop-wait
    std::thread jog([&] { driver->move_axis(1, 1.0); });              // takes Dec from the pulse
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 2, kRendezvous));
    mount.hold_running(2, false);
    // Either log line ends the wait: the unmodified driver logs the WARN, the INFO is the fixed one.
    const bool logged =
        run_clock_until(clock, [&] { return informed.load() || warned.load(); }, std::chrono::milliseconds(10000));
    jog.join();
    REQUIRE(logged);
    CHECK(informed.load());
    CHECK_FALSE(warned.load());
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

#endif  // !_WIN32
