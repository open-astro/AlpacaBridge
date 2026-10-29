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

// open-astro#436: the Sky-Watcher direct driver had no altitude limit, and a
// ConformU run drove a Wave 150i OTA into a tripod leg. These cases pin the
// goto target check over the loopback fake mount: with a floor configured, a
// goto whose target sits below it is refused with InvalidValue BEFORE any
// motion command reaches the board; with the floor off (the default) or the
// target above it, nothing changes; Park and FindHome are the recovery moves
// and stay exempt.
//
// Geometry: the fake starts at home, which points the tube at the pole
// (altitude = site latitude, 39.7 deg here). Every target below is a fraction
// of a degree from the pole at HA +5.5 h, so the gotos that must go through
// are a few seconds long. The refused cases set the floor ABOVE the pole
// (50 deg): a floor of 15 deg with a target at Dec -89 would be just as
// refused after the fix, but before it the sync form would run a 55 s goto
// to prove the point, and a 55 s RED is a bad test.
//
// The live guard (436b) cases at the end drive the axes across a limit and
// expect the driver to stop the motion on its own.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/motion_limits.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"
#include "skywatcher_fake_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
using alpacacore::test::FakeSkyWatcherMount;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::skywatcher_clock::follow_until;
using alpacacore::test::skywatcher_clock::pump_until;
using alpacacore::util::MotionLimits;

namespace {

constexpr double kLatitude = 39.7392;
constexpr double kLongitude = -104.9903;
constexpr double kElevation = 1609.0;

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount,
                                                              MotionLimits limits = {}) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, limits);
    driver->set_connected(true);
    return driver;
}

MotionLimits floor_deg(double deg) {
    MotionLimits limits;
    limits.min_altitude_deg = deg;
    return limits;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// Lets the status poll (decision 0010) sample the mount at least once after a
// test moved its axes behind the driver's back: two whole polls, so one that was
// already in flight when the jump landed is followed by one that was not. Each
// poll asks ':j' once per axis.
void settle_status_poll(FakeSkyWatcherMount& mount) {
    const int anchors = mount.frames_seen('j');
    REQUIRE(mount.wait_for_frames('j', anchors + 4, std::chrono::milliseconds(3000)));
}

// ── Fake-clock variants (open-astro#1221) ───────────────────────────────────
//
// A case whose mount runs on a FakeTaskClock (skywatcher_fake_clock.h) moves
// it only while a pump runs, so each of its waits goes through one of these.

// The same two polls, with the fake's virtual time following wall time.
void settle_status_poll(FakeTaskClock& clock, FakeSkyWatcherMount& mount) {
    const int anchors = mount.frames_seen('j');
    REQUIRE(pump_until(
        clock, mount, [&] { return mount.frames_seen('j') >= anchors + 4; }, std::chrono::milliseconds(3000)));
}

// Runs a blocking driver call (a synchronous slew) on a worker thread while
// the test thread pumps virtual time, so the mount moves under it; false when
// the call has not returned within `real_bound`. Rethrows what the call threw.
bool pump_call(FakeTaskClock& clock, FakeSkyWatcherMount& mount, const std::function<void()>& call,
               std::chrono::milliseconds real_bound) {
    std::atomic<bool> finished{false};
    std::exception_ptr error;
    std::thread worker([&] {
        try {
            call();
        } catch (...) {
            error = std::current_exception();
        }
        finished = true;
    });
    const bool done = pump_until(clock, mount, [&] { return finished.load(); }, real_bound);
    if (!done) {
        // Keep the mount moving so the call can return and the worker be joined.
        pump_until(clock, mount, [&] { return finished.load(); }, std::chrono::seconds(60));
    }
    worker.join();
    if (error) {
        std::rethrow_exception(error);
    }
    return done;
}

// A target half a degree from the pole at HA +5.5 h: altitude ~39-40 deg at
// this latitude whatever the time of day, and a short goto from home.
double near_pole_ra(const alpacacore::TelescopeDriver& driver) {
    return std::fmod(driver.get_sidereal_time() - 5.5 + 24.0, 24.0);
}
constexpr double kNearPoleDec = 89.5;

// Every command that moves, aims or re-stamps an axis. A refused goto must
// leave all of them untouched; the status polls (':f', ':j') are free to run.
struct MotionFrames {
    int mode, target, start, stop_ramped, stop_now, set_position;
    static MotionFrames snapshot(FakeSkyWatcherMount& mount) {
        return {mount.frames_seen('G'), mount.frames_seen('S'), mount.frames_seen('J'),
                mount.frames_seen('K'), mount.frames_seen('L'), mount.frames_seen('E')};
    }
    bool operator==(const MotionFrames& o) const {
        return mode == o.mode && target == o.target && start == o.start && stop_ramped == o.stop_ramped &&
               stop_now == o.stop_now && set_position == o.set_position;
    }
};

// The refusal the ASCOM vocabulary requires: InvalidValue (an out-of-range
// argument for THIS device), with a message that names the setting.
std::string require_below_floor_refusal(const std::function<void()>& fn) {
    try {
        fn();
        FAIL("Expected InvalidValue for a target below the altitude floor");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == alpacacore::AlpacaError::InvalidValue);
        return ex.what();
    }
    return {};
}

using alpacacore::test::require_alpaca_error;

}  // namespace

TEST_CASE("SkyWatcher limits - a synchronous goto below the floor is refused and nothing reaches the board",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(50.0));
    const double ra = near_pole_ra(*driver);
    const auto before = MotionFrames::snapshot(mount);

    const std::string message = require_below_floor_refusal([&] { driver->slew_to_coordinates(ra, kNearPoleDec); });
    INFO(message);
    CHECK(message.find("below the Minimum altitude limit (50 deg)") != std::string::npos);
    CHECK(message.find("device settings") != std::string::npos);

    // No motion, mode, target or position write: the refusal came before
    // dispatch_goto_locked() ever ran.
    CHECK(MotionFrames::snapshot(mount) == before);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    // And no driver state either: Slewing stays false and the target
    // properties were not seeded by the refused call.
    CHECK_FALSE(driver->get_slewing());
    require_alpaca_error([&] { driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - an asynchronous goto below the floor is refused and nothing reaches the board",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(50.0));
    const double ra = near_pole_ra(*driver);
    const auto before = MotionFrames::snapshot(mount);

    const std::string message =
        require_below_floor_refusal([&] { driver->slew_to_coordinates_async(ra, kNearPoleDec); });
    INFO(message);
    CHECK(message.find("below the Minimum altitude limit (50 deg)") != std::string::npos);
    CHECK(message.find("device settings") != std::string::npos);

    // The async initiator returns at once, so give a wrongly started task
    // time to show itself before asserting the board saw nothing.
    CHECK_FALSE(wait_until([&] { return !(MotionFrames::snapshot(mount) == before); }, 500));
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_slewing());
    require_alpaca_error([&] { driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    require_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a refused goto leaves a goto in flight running", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = near_pole_ra(*driver);
    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));

    // Dec -60 peaks at about -10 deg altitude at this latitude, so it is
    // below the floor at any time of day.
    const double low_ra = ra;
    require_alpaca_error([&] { driver->slew_to_coordinates(low_ra, -60.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(low_ra, -60.0); },
                         alpacacore::AlpacaError::InvalidValue);
    CHECK(driver->get_slewing());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(std::abs(driver->get_declination() - kNearPoleDec) < 0.05);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a legal goto during a park in flight is refused and the park completes",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(follow_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(5000)));
    const auto before = MotionFrames::snapshot(mount);

    // Above the floor, so only the parking gate can refuse it. Refusing before
    // the reap is what keeps the park running; a reap first would cancel it.
    const double ra = near_pole_ra(*driver);
    require_alpaca_error([&] { driver->slew_to_coordinates(ra, kNearPoleDec); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, kNearPoleDec); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(driver->get_slewing());
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(30)));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - FindHome during a park refuses without cancelling the park",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(follow_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(5000)));
    const auto before = MotionFrames::snapshot(mount);

    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(driver->get_slewing());
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(30)));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a later Park supersedes a synchronous slew without stale completion",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    const double ra = near_pole_ra(*driver);
    std::atomic<int> slew_result{-1};
    std::jthread sync_slew([&] {
        try {
            driver->slew_to_coordinates(ra, kNearPoleDec);
            slew_result.store(0);
        } catch (const alpacacore::AlpacaException& ex) {
            slew_result.store(ex.error_code());
        } catch (...) {
            slew_result.store(-2);
        }
    });
    REQUIRE(follow_until(
        clock, [&] { return mount.axis_in_goto(1) || mount.axis_in_goto(2); }, std::chrono::milliseconds(5000)));

    driver->park();
    REQUIRE(pump_until(clock, mount, [&] { return slew_result.load() != -1; }, std::chrono::seconds(10)));
    sync_slew.join();
    CHECK(slew_result.load() == alpacacore::AlpacaError::InvalidOperation);
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(30)));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - AbortSlew supersedes a blocking synchronous slew", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);
    auto driver = connected_driver(mount);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    const double ra = near_pole_ra(*driver);
    std::atomic<int> result{-1};
    std::jthread sync_slew([&] {
        try {
            driver->slew_to_coordinates(ra, kNearPoleDec);
            result.store(0);
        } catch (const alpacacore::AlpacaException& ex) {
            result.store(ex.error_code());
        } catch (...) {
            result.store(-2);
        }
    });
    REQUIRE(wait_until([&] { return mount.axis_in_goto(1) || mount.axis_in_goto(2); }, 5000));

    driver->abort_slew();
    sync_slew.join();
    CHECK(result.load() == alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a below-floor goto during a park in flight leaves the park running",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(follow_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(5000)));
    const auto before = MotionFrames::snapshot(mount);

    require_alpaca_error([&] { driver->slew_to_coordinates(near_pole_ra(*driver), -60.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(near_pole_ra(*driver), -60.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(30)));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a refused goto leaves a pulse guide in flight running",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->pulse_guide(0, 3000);
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    const auto before = MotionFrames::snapshot(mount);

    const double ra = near_pole_ra(*driver);
    require_alpaca_error([&] { driver->slew_to_coordinates(ra, -60.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, -60.0); }, alpacacore::AlpacaError::InvalidValue);
    // A reap would have stopped the pulse early and written stop frames.
    CHECK(driver->get_is_pulse_guiding());
    CHECK(mount.axis_running(2));
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - an out-of-range goto leaves a slew in flight running",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = near_pole_ra(*driver);
    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    const auto before = MotionFrames::snapshot(mount);

    for (const auto& bad : {std::pair<double, double>{24.5, 10.0}, std::pair<double, double>{-1.0, 10.0},
                            std::pair<double, double>{ra, 95.0}, std::pair<double, double>{ra, -95.0}}) {
        require_alpaca_error([&] { driver->slew_to_coordinates(bad.first, bad.second); },
                             alpacacore::AlpacaError::InvalidValue);
        require_alpaca_error([&] { driver->slew_to_coordinates_async(bad.first, bad.second); },
                             alpacacore::AlpacaError::InvalidValue);
    }
    CHECK(driver->get_slewing());
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(std::abs(driver->get_declination() - kNearPoleDec) < 0.05);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - Sync and MoveAxis are exempt from the floor", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(90.0));
    const double ra = near_pole_ra(*driver);
    CHECK_NOTHROW(driver->sync_to_coordinates(ra, kNearPoleDec));
    CHECK_NOTHROW(driver->move_axis(0, 0.5));
    CHECK_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - the target forms forward to the same check", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(50.0));
    driver->set_target_right_ascension(near_pole_ra(*driver));
    driver->set_target_declination(kNearPoleDec);
    const auto before = MotionFrames::snapshot(mount);

    require_below_floor_refusal([&] { driver->slew_to_target(); });
    require_below_floor_refusal([&] { driver->slew_to_target_async(); });
    CHECK_FALSE(wait_until([&] { return !(MotionFrames::snapshot(mount) == before); }, 500));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a goto above the floor is unchanged, sync and async",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = near_pole_ra(*driver);

    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(follow_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(5000)));
    REQUIRE(pump_until(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(30)));
    CHECK(std::abs(driver->get_declination() - kNearPoleDec) < 0.05);

    const int starts_before = mount.frames_seen('J');
    REQUIRE(pump_call(
        clock, mount, [&] { driver->slew_to_coordinates(ra, kNearPoleDec - 0.2); }, std::chrono::seconds(30)));
    CHECK(mount.frames_seen('J') > starts_before);
    CHECK(std::abs(driver->get_declination() - (kNearPoleDec - 0.2)) < 0.05);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - limits off means no behaviour change", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // Default-constructed MotionLimits: both fields disengaged, the shape an
    // absent or null config field produces. The same target the refused
    // cases use must now go through.
    auto driver = connected_driver(mount, MotionLimits{});
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = near_pole_ra(*driver);

    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(std::abs(driver->get_declination() - kNearPoleDec) < 0.05);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a meridian limit alone never refuses a goto", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // #432 keeps every goto target counterweight-down, so the meridian limit
    // is the live guard's business only (436b); the target check must not
    // consult it.
    MotionLimits limits;
    limits.meridian_limit_minutes = 5.0;
    auto driver = connected_driver(mount, limits);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = near_pole_ra(*driver);

    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - Park and FindHome are exempt from the floor", "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    // A floor at the zenith puts EVERY position below it, home and the
    // default park position (both the pole, altitude 39.7 deg) included.
    // They are the recovery moves: an operator must always be able to park a
    // mount or send it home, whatever the floor says.
    auto driver = connected_driver(mount, floor_deg(90.0));
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);

    // FindHome first: it early-returns when the driver already believes it is
    // at home, which a completed Park would make true.
    CHECK_NOTHROW(driver->find_home());
    REQUIRE(driver->get_slewing());
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_home(); }, std::chrono::seconds(60)));
    CHECK(mount.frames_seen('J') >= 2);

    mount.jump_axis_degrees(1, 25.0);
    mount.jump_axis_degrees(2, 15.0);
    const int starts_before = mount.frames_seen('J');
    CHECK_NOTHROW(driver->park());
    REQUIRE(pump_until(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(30)));
    CHECK(mount.frames_seen('J') > starts_before);
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());

    // The floor still applies to a goto from the same connected driver, so
    // the exemption is Park/FindHome's, not the session's.
    require_below_floor_refusal([&] { driver->slew_to_coordinates_async(near_pole_ra(*driver), kNearPoleDec); });
    driver->set_connected(false);
}

// ── Live guard (436b) ───────────────────────────────────────────────────────
//
// Geometry: with the RA axis at home (a1 = 0) the tube sits six hours of hour
// angle from the meridian, where altitude depends on declination alone:
// sin(alt) = sin(dec) sin(lat). Dec axis at 30 deg is dec 60, altitude 33.6
// at this latitude whatever the time of day; driving that axis further out
// lowers the tube about 0.4 deg of altitude per degree of axis. The meridian
// limit is read off the RA axis alone: a1 = 92 deg is the counterweight 2 deg
// above horizontal, 8 minutes past the meridian.

namespace {

constexpr double kGuardDecAxisDeg = 30.0;
constexpr double kGuardAltitudeDeg = 33.6;                   // at kGuardDecAxisDeg, see above
const double kMaxMoveAxisRate = 800.0 * 360.0 / 86164.0905;  // the advertised AxisRates maximum
// MoveAxis is sky-referenced (open-astro#297). These cases sit north of the
// equator on the a2 > 0 Dec branch, where a positive secondary rate raises
// Dec and so turns the axis DOWN: the cases that drive the axis outward
// (lowering the tube) therefore pass a negative rate.

MotionLimits meridian_minutes(double minutes) {
    MotionLimits limits;
    limits.meridian_limit_minutes = minutes;
    return limits;
}

}  // namespace

TEST_CASE("SkyWatcher limits - MoveAxis carrying the tube below the floor is stopped",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // Before the connect, which warms the position cache: a jump after it
    // would not be read back until the cache expires.
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.2);

    driver->move_axis(1, -kMaxMoveAxisRate);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    // About 1.4 deg of altitude per second: the floor is reached in about a
    // second, and nothing but the guard stops the axis before the timeout.
    CHECK(wait_until([&] { return !mount.axis_running(2); }, 6000));
    CHECK(wait_until([&] { return !driver->get_slewing(); }, 6000));
    // Stopped at the floor, not somewhere far below it: one 250 ms poll and
    // the stop ramp past the crossing.
    const double altitude = driver->get_altitude();
    INFO("altitude after the stop: " << altitude);
    CHECK(altitude < kGuardAltitudeDeg - 1.5);
    CHECK(altitude > kGuardAltitudeDeg - 3.0);
    driver->set_connected(false);
}

// The guard reads the polled frame only while it is fresh. With the poll
// stalled the frame is stale, so the guard must keep its direct read of the
// board: serving the frozen frame would leave it blind to the moving axis.
TEST_CASE("SkyWatcher limits - a stale frame is not served to the guard and it reads the board",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.2);

    // A slow MoveAxis: the dead-reckoned model alone never reaches the floor
    // in this test, so only a direct read of the board can see the jump.
    driver->move_axis(1, -kMaxMoveAxisRate / 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    sw::set_status_poll_paused_for_testing(*driver, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // past the freshness bound
    const int reads_before = mount.frames_seen('j');
    mount.jump_axis_degrees(2, kGuardDecAxisDeg + 10.0);  // about 4 deg below the floor, behind the frozen frame
    CHECK(wait_until([&] { return !mount.axis_running(2); }, 6000));
    CHECK(mount.frames_seen('j') > reads_before);
    sw::set_status_poll_paused_for_testing(*driver, false);
    CHECK(wait_until([&] { return !driver->get_slewing(); }, 6000));
    driver->set_connected(false);
}

// No getter clears the slewing flag (decision 0010): a synchronous slew that a
// MoveAxis supersedes clears it, or the guard lets the MoveAxis cross the floor.
TEST_CASE("SkyWatcher limits - a MoveAxis that supersedes a synchronous slew is guarded",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.2);
    const double ra = driver->get_right_ascension();  // same hour angle: a Dec-only goto that raises the tube
    std::atomic<int> result{-1};
    std::jthread sync_slew([&] {
        try {
            driver->slew_to_coordinates(ra, 80.0);
            result.store(0);
        } catch (const alpacacore::AlpacaException& ex) {
            result.store(ex.error_code());
        } catch (...) {
            result.store(-2);
        }
    });
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 5000));

    driver->move_axis(1, -kMaxMoveAxisRate);
    sync_slew.join();
    CHECK(result.load() == alpacacore::AlpacaError::InvalidOperation);
    CHECK(wait_until([&] { return !mount.axis_running(2); }, 10000));
    CHECK(driver->get_altitude() < kGuardAltitudeDeg - 1.5);
    driver->set_connected(false);
}

// Tracking off supersedes the slew without touching its flags, so the slew ends
// them itself, or Slewing reads true for the rest of the session.
TEST_CASE("SkyWatcher limits - Tracking off that supersedes a synchronous slew lets Slewing end",
          "[skywatcher][telescope][limits]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    mount.jump_axis_degrees(2, 20.0);
    driver->set_tracking(true);
    const double ra = near_pole_ra(*driver);
    std::atomic<int> result{-1};
    std::jthread sync_slew([&] {
        try {
            driver->slew_to_coordinates(ra, kNearPoleDec);
            result.store(0);
        } catch (const alpacacore::AlpacaException& ex) {
            result.store(ex.error_code());
        } catch (...) {
            result.store(-2);
        }
    });
    // the goto: Dec does not track
    REQUIRE(follow_until(clock, [&] { return mount.axis_running(2); }, std::chrono::milliseconds(5000)));

    driver->set_tracking(false);
    REQUIRE(pump_until(clock, mount, [&] { return result.load() != -1; }, std::chrono::seconds(10)));
    sync_slew.join();
    CHECK(result.load() == alpacacore::AlpacaError::InvalidOperation);
    // the board ends the Dec goto
    REQUIRE(pump_until(clock, mount, [&] { return !mount.axis_running(2); }, std::chrono::seconds(15)));
    settle_status_poll(clock, mount);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// The mount crosses the floor between the position the driver last knew and
// the guard's first sample (the cache still holds the inside position, the
// board is already outside). The baseline must come from before the dispatch,
// or the outside first sample is accepted unchecked and nothing stops the axis.
// The driver shares the mount's FakeTaskClock: its status poll and the position
// cache's TTL read that clock, so with virtual time standing still from the
// jump to the command no poll can refresh the cache in between (on the wall
// clock one could, open-astro#1796). No goto runs, so pump_until() only lets
// virtual time follow wall time.
TEST_CASE("SkyWatcher limits - a floor crossed before the guard's first sample is stopped",
          "[skywatcher][telescope][limits][guard]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {},
                                                  floor_deg(kGuardAltitudeDeg - 1.5), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::milliseconds(2000)));  // the status poll, parked
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.2);
    // Age the polled frame past its freshness bound (still inside the cache's
    // TTL), so the guard's first sample reads the board, not the inside frame.
    sw::set_status_poll_paused_for_testing(*driver, true);
    clock.advance(std::chrono::milliseconds(300));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::milliseconds(2000)));

    mount.jump_axis_degrees(2, kGuardDecAxisDeg + 10.0);  // about 4 deg below the floor
    driver->move_axis(1, -kMaxMoveAxisRate);
    sw::set_status_poll_paused_for_testing(*driver, false);
    REQUIRE(follow_until(clock, [&] { return mount.axis_running(2); }, std::chrono::milliseconds(2000)));
    CHECK(pump_until(clock, mount, [&] { return !mount.axis_running(2); }, std::chrono::seconds(6)));
    CHECK(pump_until(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(6)));
    driver->set_connected(false);
}

// Tracking start from rest takes the same pre-dispatch baseline as MoveAxis:
// the cache holds the inside position, the board is already past the limit.
// The driver and the mount share one FakeTaskClock, as in the floor case above.
TEST_CASE("SkyWatcher limits - a meridian limit crossed before tracking's first sample is stopped",
          "[skywatcher][telescope][limits][guard]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(1, 92.0);  // 8 min past the meridian: inside the limit
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {},
                                                  meridian_minutes(10.0), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::milliseconds(2000)));  // the status poll, parked

    mount.jump_axis_degrees(1, 94.0);  // 16 min past the meridian, before the dispatch
    driver->set_tracking(true);
    CHECK(pump_until(clock, mount, [&] { return !driver->get_tracking(); }, std::chrono::seconds(8)));
    CHECK(pump_until(clock, mount, [&] { return !mount.axis_running(1); }, std::chrono::seconds(6)));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - tracking past the meridian limit stops tracking",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(1, 92.0);  // 8 min past the meridian: inside the limit
    auto driver = connected_driver(mount, meridian_minutes(10.0));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    // Let the guard take its first sample inside the limit, then carry the
    // axis past it: tracking alone would take minutes to cover the distance.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    mount.jump_axis_degrees(1, 94.0);  // 16 min past the meridian

    CHECK(wait_until([&] { return !driver->get_tracking(); }, 8000));
    CHECK(wait_until([&] { return !mount.axis_running(1); }, 6000));
    driver->set_connected(false);
}

// Sync moves the frame, not the axes: the guard must not read the jump in the
// reported position as motion across a limit. The floor carries the case: a
// sync always lands counterweight-down (|a1| <= 90), so it can never put the
// frame past a meridian limit.
TEST_CASE("SkyWatcher limits - a sync below the floor does not stop tracking",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    // The guard's first sample, above the floor, is its baseline.
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Ten degrees less declination at the same hour angle: about 29 deg of
    // altitude, below the floor, with neither axis moved.
    driver->sync_to_coordinates(driver->get_right_ascension(), driver->get_declination() - 10.0);
    const double altitude = driver->get_altitude();
    INFO("altitude after the sync: " << altitude);
    REQUIRE(altitude < kGuardAltitudeDeg - 3.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // two 2 s polls
    CHECK(driver->get_tracking());
    CHECK(mount.axis_running(1));
    driver->set_connected(false);
}

// A Tracking=false whose stop-wait threw leaves Tracking true until a poll
// sees RA stopped. A slew that starts first takes the axes and will restore
// tracking at the end: a Tracking read while it holds RA stopped must not
// settle the old stop, or the guard exits on its next poll and the restored
// tracking runs with no limit enforced.
TEST_CASE("SkyWatcher limits - a slew after a Tracking=false stop that threw keeps the guard running",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(-80.0));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));

    mount.hold_running(1, true);
    CHECK_THROWS(driver->set_tracking(false));  // the 5 s stop timeout
    mount.hold_running(1, false);
    REQUIRE(wait_until([&] { return !mount.axis_running(1); }, 3000));

    // No Tracking read before the slew: the stop is still unconfirmed. Same
    // hour angle, 30 deg of declination: RA lands at once, Dec runs ~9 s.
    driver->slew_to_coordinates_async(driver->get_right_ascension(), 60.0);
    bool saw_ra_stopped = false;
    bool tracking_read_false = false;
    while (driver->get_slewing()) {
        const bool ra_stopped = !mount.axis_running(1);
        if (!driver->get_tracking()) {
            tracking_read_false = true;
        }
        saw_ra_stopped = saw_ra_stopped || ra_stopped;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(saw_ra_stopped);
    CHECK_FALSE(tracking_read_false);
    CHECK(driver->get_tracking());
    CHECK(wait_until([&] { return mount.axis_running(1); }, 3000));
    // The guard body started by the first Tracking=true is still the one
    // watching: the post-slew restore does not start another.
    CHECK(sw::detail::limit_guard_bodies_running() == 1);
    driver->set_tracking(false);
    driver->set_connected(false);
}

// AbortSlew during a MoveAxis restores tracking (ITelescopeV4), but publishes
// Tracking false first and then waits on the RA stop with mutex_ released. A
// guard polling in that window (every 250 ms during a jog) exits, and the
// restored tracking then ran with no limit enforced.
TEST_CASE("SkyWatcher limits - AbortSlew restoring tracking after a MoveAxis keeps the guard running",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(-80.0));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));

    driver->move_axis(1, 2.0);  // a Dec jog: RA keeps tracking
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));

    // RA keeps reporting running for a second after the instant stop, so the
    // restore's stop-wait spans several guard polls.
    mount.hold_running(1, true);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        mount.hold_running(1, false);
    });
    driver->abort_slew();
    release.join();

    REQUIRE(driver->get_tracking());
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(sw::detail::limit_guard_bodies_running() == 1);
    driver->set_tracking(false);
    driver->set_connected(false);
}

// The guard re-anchors the position cache on hardware every 2 s while
// tracking. An RA guide pulse that changed the dead-reckoning rate without
// re-anchoring then lost the pulse distance driven since that anchor: the read
// after the pulse extrapolated the whole interval at the restored rate, and
// the next hardware read jumped back (EQM-35 Pro ConformU with a floor set:
// PulseGuide RA change short by up to 1.07 s of the 2.51 s expected).
TEST_CASE("SkyWatcher limits - an RA guide pulse under the guard keeps the reported RA continuous",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // Far below anything the tube reaches: the guard polls but never fires.
    auto driver = connected_driver(mount, floor_deg(-80.0));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    const uint32_t sidereal_preset = mount.step_period(1);
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));

    // Nothing but the guard reads the position from here on. Start the pulse
    // just after one of its hardware anchors: the poll 4 s later re-anchors
    // (whatever the pulse dispatch did to the cache) about a second before
    // the pulse ends, and the read below falls inside that anchor's lifetime.
    const int anchors = mount.frames_seen('j');
    REQUIRE(mount.wait_for_frames('j', anchors + 1, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    driver->pulse_guide(2, 5000);  // East, in place on the tracking axis: ConformU's length
    REQUIRE(wait_until([&] { return mount.step_period(1) != sidereal_preset; }, 2000));
    REQUIRE(wait_until([&] { return mount.step_period(1) == sidereal_preset; }, 6000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double ra_after_pulse = driver->get_right_ascension();

    // Past the cache lifetime: this read re-anchors on the axis counts.
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    const double ra_reanchored = driver->get_right_ascension();
    const double jump_arcsec = (ra_reanchored - ra_after_pulse) * 15.0 * 3600.0;
    INFO("RA jump at the re-anchor: " << jump_arcsec << " arcsec");
    // Tracking holds RA still; a lost second of a 0.5x guide pulse is 7.5".
    CHECK(std::abs(jump_arcsec) < 2.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - motion that starts outside a limit is not stopped",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    MotionLimits limits = floor_deg(kGuardAltitudeDeg + 5.0);  // the tube starts below it
    limits.meridian_limit_minutes = 10.0;
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, limits);
    REQUIRE(driver->get_altitude() < kGuardAltitudeDeg + 5.0);

    // Further below the floor: recovery moves out of a limit must stay
    // possible, and so must any move that never crossed one.
    driver->move_axis(1, -kMaxMoveAxisRate);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(mount.axis_running(2));
    CHECK(driver->get_slewing());
    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 6000));

    // Already past the meridian limit when tracking starts.
    mount.jump_axis_degrees(1, 95.0);
    // The guard's baseline comes from the position cache, valid for 2 s after
    // the last poll: a jump inside that window is invisible to it and the
    // stale in-limit sample reads as a crossing once the guard polls.
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    mount.jump_axis_degrees(1, 96.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // two 2 s polls
    CHECK(driver->get_tracking());
    CHECK(mount.axis_running(1));
    driver->set_connected(false);
}

// A guard body sleeps up to a whole poll before it sees its motion end, and
// a newer motion supersedes it in that window. The newer body must not take
// the finished motion's last sample as its baseline: here that sample is
// inside the meridian limit and tracking restarts past it, which reads as a
// crossing although nothing carried the axis across one while guarded.
TEST_CASE("SkyWatcher limits - a motion started from rest takes no baseline from the last one",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, meridian_minutes(10.0));  // RA axis home: inside
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));
    // Just after one of the guard's 2 s polls, so the next one is far off:
    // the restart below lands while the old body still sleeps.
    const int anchors = mount.frames_seen('j');
    REQUIRE(mount.wait_for_frames('j', anchors + 1, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    driver->set_tracking(false);
    mount.jump_axis_degrees(1, 95.0);  // 20 min past the meridian
    settle_status_poll(mount);         // the status poll takes the jump into the model
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // two 2 s polls
    CHECK(driver->get_tracking());
    CHECK(mount.axis_running(1));
    driver->set_connected(false);
}

// The same window, closed by a MoveAxis instead of a Tracking restart.
TEST_CASE("SkyWatcher limits - a MoveAxis started from rest takes no baseline from tracking",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, meridian_minutes(10.0));
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));
    const int anchors = mount.frames_seen('j');
    REQUIRE(mount.wait_for_frames('j', anchors + 1, std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    driver->set_tracking(false);
    mount.jump_axis_degrees(1, 95.0);
    settle_status_poll(mount);
    driver->move_axis(1, -kMaxMoveAxisRate / 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // four 250 ms polls
    CHECK(mount.axis_running(2));
    CHECK(driver->get_slewing());
    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 6000));
    driver->set_connected(false);
}

// Tracking on, then MoveAxis on the Dec axis across a floor 1.5 deg below the
// start, with every ":K" from here on ramping for 7 s: longer than the 5 s the
// tracking stop waits for the RA axis, so that stop always times out.
// Returns the Dec and RA stop counts at the moment the Dec axis started.
std::pair<int, int> start_guarded_move_with_slow_stops(FakeSkyWatcherMount& mount,
                                                       alpacacore::TelescopeDriver& driver) {
    driver.set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    mount.set_stop_ramp_ms(7000);
    driver.move_axis(1, -kMaxMoveAxisRate);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    return {mount.stop_count(2), mount.stop_count(1)};
}

TEST_CASE("SkyWatcher limits - a tracking stop that times out does not hold back the MoveAxis stop",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    const auto [dec_stops, ra_stops] = start_guarded_move_with_slow_stops(mount, *driver);
    static_cast<void>(ra_stops);

    // The floor is reached in about a second; the Dec stop must follow at
    // the next poll, not after the tracking stop's 5 s wait gives up.
    CHECK(wait_until([&] { return mount.stop_count(2) > dec_stops; }, 3000));
    mount.set_stop_ramp_ms(0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a guard stop that fails is retried on the next poll",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    const auto [dec_stops, ra_stops] = start_guarded_move_with_slow_stops(mount, *driver);
    static_cast<void>(dec_stops);

    // The first tracking stop has reached the RA axis and will time out on
    // its 7 s ramp. A stop sent from now on lands at once, so only a retry
    // can turn tracking off.
    REQUIRE(wait_until([&] { return mount.stop_count(1) > ra_stops; }, 4000));
    mount.set_stop_ramp_ms(0);
    CHECK(wait_until([&] { return !driver->get_tracking(); }, 12000));
    CHECK_FALSE(mount.axis_running(1));
    driver->set_connected(false);
}

// The guard's order (Dec MoveAxis stop, then Tracking=false), driven by hand:
// the Dec stop task lands while the tracking stop still waits on a slow RA
// ramp, and must not restore the DeclinationRate offset into that wait.
TEST_CASE("SkyWatcher limits - a MoveAxis Dec stop does not restore the Dec rate into a tracking stop",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    driver->set_declination_rate(30.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1) && mount.axis_running(2); }, 2000));
    driver->move_axis(1, -kMaxMoveAxisRate);
    REQUIRE(driver->get_slewing());

    mount.set_stop_ramp_ms(300);  // the Dec stop lands first ...
    driver->move_axis(1, 0.0);
    mount.set_stop_ramp_ms(2000);  // ... inside the RA stop-wait
    CHECK_NOTHROW(driver->set_tracking(false));
    CHECK_FALSE(driver->get_tracking());
    mount.set_stop_ramp_ms(0);
    CHECK(wait_until([&] { return !mount.axis_running(2); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

// A position getter fills the cache while an axis still ramps down after
// MoveAxis(axis, 0); the mount coasts past the floor and the stop completes
// inside the cache lifetime. The next MoveAxis from rest must seed its guard
// baseline from the board, not from that pre-coast cache: a reverse move that
// drives back inside would otherwise read the outside first sample as a
// crossing and stop itself.
TEST_CASE("SkyWatcher limits - a reverse MoveAxis after a stop does not seed from a mid-ramp cache",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.jump_axis_degrees(2, kGuardDecAxisDeg);
    auto driver = connected_driver(mount, floor_deg(kGuardAltitudeDeg - 1.5));
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.2);

    mount.set_stop_ramp_ms(800);
    driver->move_axis(1, -kMaxMoveAxisRate / 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    driver->move_axis(1, 0.0);
    REQUIRE(driver->get_slewing());
    REQUIRE(std::abs(driver->get_altitude() - kGuardAltitudeDeg) < 0.5);  // fills the cache mid-ramp

    mount.set_stop_ramp_ms(0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 3000));
    mount.jump_axis_degrees(2, kGuardDecAxisDeg + 10.0);  // coasted about 4 deg past the floor

    driver->move_axis(1, kMaxMoveAxisRate / 10.0);  // back inside
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // four 250 ms polls
    CHECK(mount.axis_running(2));
    CHECK(driver->get_slewing());
    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 6000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - with no limit set the guard never starts", "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    const auto started = sw::detail::limit_guard_bodies_started();

    driver->move_axis(1, -kMaxMoveAxisRate);
    driver->set_tracking(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(sw::detail::limit_guard_bodies_started() == started);
    CHECK(sw::detail::limit_guard_bodies_running() == 0);
    driver->move_axis(1, 0.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - disconnect during a guarded MoveAxis joins the guard",
          "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    // Far below anything the axis reaches: the guard runs but never fires.
    auto driver = connected_driver(mount, floor_deg(-80.0));
    driver->move_axis(0, kMaxMoveAxisRate);
    REQUIRE(wait_until([&] { return sw::detail::limit_guard_bodies_running() == 1; }, 2000));

    driver->set_connected(false);
    // Joined, not merely asked to stop: the body sleeps 250 ms between
    // polls, so a disconnect that only flagged it would return first.
    CHECK(sw::detail::limit_guard_bodies_running() == 0);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(driver->get_connected());
    driver.reset();
    CHECK(sw::detail::limit_guard_bodies_running() == 0);
}

TEST_CASE("SkyWatcher tracking guard - all four equatorial slews are refused while Tracking is false",
          "[skywatcher][telescope][tracking-guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE_FALSE(driver->get_tracking());
    const double ra = near_pole_ra(*driver);
    const auto before = MotionFrames::snapshot(mount);

    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_alpaca_error([&] { driver->slew_to_coordinates(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, 95.0); }, alpacacore::AlpacaError::InvalidValue);

    require_alpaca_error([&] { driver->slew_to_coordinates(ra, kNearPoleDec); },
                         alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(ra, kNearPoleDec); },
                         alpacacore::AlpacaError::InvalidOperation);
    driver->set_target_right_ascension(ra);
    driver->set_target_declination(kNearPoleDec);
    require_alpaca_error([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_alpaca_error([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // The async forms return at once, so give a wrongly started task time to show itself.
    CHECK_FALSE(wait_until([&] { return !(MotionFrames::snapshot(mount) == before); }, 500));
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_slewing());

    // With Tracking on the same call goes through.
    driver->set_tracking(true);
    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    CHECK(driver->get_slewing());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    driver->set_connected(false);
}

#endif  // _WIN32
