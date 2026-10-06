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
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;
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

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected an AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == expected_code);
    }
}

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
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
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
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - FindHome during a park refuses without cancelling the park",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    const auto before = MotionFrames::snapshot(mount);

    require_alpaca_error([&] { driver->find_home(); }, alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(driver->get_slewing());
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a later Park supersedes a synchronous slew without stale completion",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
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
    REQUIRE(wait_until([&] { return mount.axis_running(1) || mount.axis_running(2); }, 5000));

    driver->park();
    sync_slew.join();
    CHECK(slew_result.load() == alpacacore::AlpacaError::InvalidOperation);
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - AbortSlew supersedes a blocking synchronous slew", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);
    auto driver = connected_driver(mount);
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
    REQUIRE(wait_until([&] { return mount.axis_running(1) || mount.axis_running(2); }, 5000));

    driver->abort_slew();
    sync_slew.join();
    CHECK(result.load() == alpacacore::AlpacaError::InvalidOperation);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - a below-floor goto during a park in flight leaves the park running",
          "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    mount.jump_axis_degrees(1, 30.0);
    mount.jump_axis_degrees(2, 20.0);
    driver->park();
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    const auto before = MotionFrames::snapshot(mount);

    require_alpaca_error([&] { driver->slew_to_coordinates(near_pole_ra(*driver), -60.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    require_alpaca_error([&] { driver->slew_to_coordinates_async(near_pole_ra(*driver), -60.0); },
                         alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(MotionFrames::snapshot(mount) == before);
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
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
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, floor_deg(15.0));
    const double ra = near_pole_ra(*driver);

    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(std::abs(driver->get_declination() - kNearPoleDec) < 0.05);

    const int starts_before = mount.frames_seen('J');
    driver->slew_to_coordinates(ra, kNearPoleDec - 0.2);
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
    const double ra = near_pole_ra(*driver);

    driver->slew_to_coordinates_async(ra, kNearPoleDec);
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return mount.frames_seen('J') >= 2; }, 5000));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher limits - Park and FindHome are exempt from the floor", "[skywatcher][telescope][limits]") {
    FakeSkyWatcherMount mount;
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
    REQUIRE(wait_until([&] { return driver->get_at_home(); }, 60000));
    CHECK(mount.frames_seen('J') >= 2);

    mount.jump_axis_degrees(1, 25.0);
    mount.jump_axis_degrees(2, 15.0);
    const int starts_before = mount.frames_seen('J');
    CHECK_NOTHROW(driver->park());
    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 30000));
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

    driver->move_axis(1, kMaxMoveAxisRate);
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
    driver->move_axis(1, kMaxMoveAxisRate);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(mount.axis_running(2));
    CHECK(driver->get_slewing());
    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 6000));

    // Already past the meridian limit when tracking starts.
    mount.jump_axis_degrees(1, 95.0);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    mount.jump_axis_degrees(1, 96.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));  // two 2 s polls
    CHECK(driver->get_tracking());
    CHECK(mount.axis_running(1));
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
    driver.move_axis(1, kMaxMoveAxisRate);
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
    driver->move_axis(1, kMaxMoveAxisRate);
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

TEST_CASE("SkyWatcher limits - with no limit set the guard never starts", "[skywatcher][telescope][limits][guard]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    const auto started = sw::detail::limit_guard_bodies_started();

    driver->move_axis(1, kMaxMoveAxisRate);
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

#endif  // _WIN32
