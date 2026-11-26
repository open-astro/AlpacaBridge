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

// open-astro#575: an async slew that fails AFTER the initiator returned used
// to be logged and forgotten -- the next Slewing read answered false, which a
// client cannot tell apart from a successful arrival. The ASCOM completion
// property contract (.claude/skills/ascom-alpaca-protocol/references/
// async-and-errors.md, "Completion property") requires the next Slewing read
// to return an Alpaca error instead, kept until a later valid command
// (AbortSlew, a new slew initiator, disconnect) clears it.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/motion_limits.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"

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

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount,
                                                              alpacacore::test::FakeTaskClock& clock) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0, {}, {}, clock);
    driver->set_connected(true);
    return driver;
}

constexpr auto kClockStep = std::chrono::milliseconds(50);
constexpr auto kRendezvous = std::chrono::milliseconds(2000);
// A connected driver keeps its status poll (decision 0010) parked on the clock
// between polls: one waiter that is always there, counted before any task a
// case is waiting for.
constexpr std::size_t kPollerWaiter = 1;

// Moves the shared fake clock forward in steps, once a driver thread is parked
// on it, until pred() holds; false when `budget` of virtual time passes first.
// A slew task's polls and the mount's motion both run on that clock, so nothing
// here sleeps for real time.
bool run_clock_until(alpacacore::test::FakeTaskClock& clock, const std::function<bool()>& pred,
                     std::chrono::milliseconds budget) {
    for (std::chrono::milliseconds done{0}; done < budget; done += kClockStep) {
        if (pred()) {
            return true;
        }
        if (clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous)) {
            clock.advance(kClockStep);
            clock.wait_for_woken_settled(kRendezvous);
        }
    }
    return pred();
}

// Threw only for the driver-specific DriverException (0x500) the #575 contract
// requires; any other code is ThrewOther, so a changed code fails the tests.
enum class SlewingRead : std::uint8_t { True, False, Threw, ThrewOther };

SlewingRead read_slewing(alpacacore::TelescopeDriver& driver, std::string* message = nullptr) {
    try {
        return driver.get_slewing() ? SlewingRead::True : SlewingRead::False;
    } catch (const alpacacore::AlpacaException& ex) {
        if (message) *message = ex.what();
        return ex.error_code() == alpacacore::AlpacaError::DriverException ? SlewingRead::Threw
                                                                           : SlewingRead::ThrewOther;
    }
}

}  // namespace

TEST_CASE("SkyWatcher async - a slew that fails after dispatch surfaces through Slewing (#575)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    // The goto starts RA first; refusing that ":J" with "!2" (Motor not
    // stopped) makes dispatch_predicted_goto_locked() throw INSIDE the slew
    // task, after slew_to_coordinates_async() has already returned.
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    mount.reject_start_motion(1, 1);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));

    // Today the task logs "Async slew failed" and Slewing settles to FALSE --
    // indistinguishable from a landed goto. It must instead become an error.
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);  // names the ASCOM operation

    // Preserved until a client-visible clearing command: a second poller
    // (web UI, ConformU's 500 ms loop) must see the same failure, not false.
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // AbortSlew is a valid later command: it clears the stored failure and
    // Slewing reads normally again.
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);

    // And a fresh async slew (the fake accepts this one) is a clean start:
    // Slewing=true with no stale error in the way, landing as usual.
    // The failed goto left Tracking off, so the client switches it on again first.
    driver->set_tracking(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) == SlewingRead::False; }, std::chrono::milliseconds(30000)));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a goto that fails after RA started stops the axes (#2407)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    // RA's goto starts, then Dec's ":J" is refused: the body throws with RA
    // still on its way, five hours of RA from the target.
    mount.reject_start_motion(2, 1);
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK(message.find("SlewToCoordinatesAsync failed") != std::string::npos);
    // Slewing no longer claims motion, so nothing may still be moving.
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_tracking());  // the stop ended the drive too
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a goto that times out with an axis still running stops it (#2407)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    // Both axes are on their way: Dec then stalls, running in goto mode
    // without moving until a stop clears it, so the 180 s wait gives up.
    REQUIRE(run_clock_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(10000)));
    mount.set_axis_running(2, true, /*speed_mode=*/false);
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(200000)));
    CHECK(message.find("Slew timed out") != std::string::npos);
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(mount.axis_running(1));
    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a new slew initiator replaces the stored failure (#575)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);

    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    mount.reject_start_motion(1, 1);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) == SlewingRead::Threw; }, std::chrono::milliseconds(10000)));

    // No AbortSlew in between: the next initiator itself clears the failure
    // (a client that retries the goto must not be told the OLD goto failed).
    // The failure stopped both axes and with them the drive (open-astro#2407),
    // so the client switches Tracking on again first; that does not clear it.
    driver->set_tracking(true);
    REQUIRE(read_slewing(*driver) == SlewingRead::Threw);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) == SlewingRead::False; }, std::chrono::milliseconds(30000)));
    driver->set_connected(false);
}

// GUARD, not a proof: this case is GREEN before the fix and stays green
// after it. It pins the regression risk named in the #575 design: AbortSlew
// cancels the slew task by making wait_for_slew_complete() throw "Slew wait
// cancelled" into the SAME catch block that must now record real failures.
// A fix that records the cancellation as a failure turns this red.
TEST_CASE("SkyWatcher async - AbortSlew's own cancellation is not reported as a slew failure (#575)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    REQUIRE(read_slewing(*driver) == SlewingRead::True);
    // The slew task is parked on the clock (its goto dispatch / polls).
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));
    clock.advance(std::chrono::milliseconds(400));
    clock.wait_for_woken_settled(kRendezvous);
    driver->abort_slew();  // joins the slew task: its catch block has run by now
    CHECK(read_slewing(*driver) == SlewingRead::False);
    // Aborted: nothing is parked on the clock now, so only the mount moves.
    clock.advance(std::chrono::milliseconds(500));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - Park dispatch failure surfaces through Slewing and leaves AtPark false",
          "[skywatcher][async][parkfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    // RA starts, then DEC dispatch fails: cleanup must stop both axes.
    mount.reject_start_motion(2, 1);

    REQUIRE_NOTHROW(driver->park());
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK(message.find("Park failed") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    CHECK_FALSE(driver->get_at_park());
    CHECK(mount.stop_count(1) > 0);
    CHECK(mount.stop_count(2) > 0);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - AutoHome failure surfaces through Slewing and leaves AtHome false",
          "[skywatcher][async][homefailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.reject_start_motion(1, 1);
    mount.reject_instant_stop(1, 1);

    REQUIRE_NOTHROW(driver->find_home());
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK(message.find("FindHome failed") != std::string::npos);
    CHECK(message.find("stopping after failure failed") != std::string::npos);
    CHECK(message.find("axis 1") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);
    CHECK_FALSE(driver->get_at_home());
    CHECK(mount.stop_count(2) > 0);  // DEC stop is still attempted after the RA stop fails.
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - count-frame FindHome failure persists through Slewing",
          "[skywatcher][async][homefailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.reject_start_motion(1, 1);

    REQUIRE_NOTHROW(driver->find_home());
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK(message.find("FindHome failed") != std::string::npos);
    CHECK_FALSE(driver->get_at_home());
    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - count-frame FindHome rejects a short landing", "[skywatcher][async][homefailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 5.0);
    mount.land_short_by(1, 1, 0.2, 60000);

    REQUIRE_NOTHROW(driver->find_home());
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    CHECK(message.find("FindHome count-frame slew did not reach home") != std::string::npos);
    CHECK_FALSE(driver->get_at_home());
    CHECK(std::abs(mount.axis_degrees(1)) > 0.1);

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

// open-astro#2552: a MoveAxis takes no slew-slot claim, so it does not supersede a FindHome body, and a rate-0
// MoveAxis on an axis with no jog commands nothing at all. Neither may hide the body's later failure. Both used to
// start a new failure epoch, and the body's failure path then returned before it dropped homing_: Slewing read
// true with nothing moving, the failure was lost, and the failure stop never ran. (A Park cannot meet this: a
// MoveAxis while parking is refused with InvalidWhileParked before it touches the epoch.)
// Only Dec has a way to go here, and it stalls in goto mode, so the body's 180 s wait gives up with Dec still
// running; the MoveAxis drives the idle RA axis (a jog, started and stopped) or commands nothing (the no-op).
namespace {

// The body's 180 s wait started before this budget does, so it times out inside it. Kept tight: once the body has
// ended, each step waits out the clock rendezvous in real time, so a red run would otherwise take an hour.
constexpr auto kPastSlewTimeout = std::chrono::milliseconds(181000);

enum class MidBodyMoveAxis : std::uint8_t { NoOp, Jog };

void move_axis_on_clock(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver, int axis,
                        double rate) {
    auto call = std::async(std::launch::async, [&] { driver.move_axis(axis, rate); });
    REQUIRE(run_clock_until(
        clock, [&] { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; },
        std::chrono::milliseconds(10000)));
    call.get();
}

void mid_body_move_axis(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                        FakeSkyWatcherMount& mount, MidBodyMoveAxis move) {
    if (move == MidBodyMoveAxis::NoOp) {
        move_axis_on_clock(clock, driver, 0, 0.0);
        return;
    }
    move_axis_on_clock(clock, driver, 0, 1.0);
    REQUIRE(mount.axis_running(1));
    move_axis_on_clock(clock, driver, 0, 0.0);
    REQUIRE(run_clock_until(clock, [&] { return !mount.axis_running(1); }, std::chrono::milliseconds(10000)));
}

void check_home_failure_after_move_axis(MidBodyMoveAxis move) {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(2, 5.0);
    REQUIRE_NOTHROW(driver->find_home());
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(2); }, std::chrono::milliseconds(10000)));
    mount.set_axis_running(2, true, /*speed_mode=*/false);
    mid_body_move_axis(clock, *driver, mount, move);
    REQUIRE(read_slewing(*driver) == SlewingRead::True);  // the body runs on

    std::string message;
    SlewingRead last = SlewingRead::True;
    REQUIRE(run_clock_until(
        clock,
        [&] {
            last = read_slewing(*driver, &message);
            return last != SlewingRead::True;
        },
        kPastSlewTimeout));
    REQUIRE(last == SlewingRead::Threw);  // the failure is kept, not a silent false
    CHECK(message.find("FindHome failed") != std::string::npos);
    CHECK(message.find("Slew timed out") != std::string::npos);
    CHECK_FALSE(mount.axis_running(2));  // the failure stop ran
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(driver->get_at_home());
    // The phase flag is down: once AbortSlew clears the failure, Slewing reads false.
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

}  // namespace

TEST_CASE("SkyWatcher async - a FindHome that fails after a rate-0 MoveAxis keeps its failure (#2552)",
          "[skywatcher][async][homefailure]") {
    check_home_failure_after_move_axis(MidBodyMoveAxis::NoOp);
}

TEST_CASE("SkyWatcher async - a FindHome that fails after a jog keeps its failure (#2552)",
          "[skywatcher][async][homefailure]") {
    check_home_failure_after_move_axis(MidBodyMoveAxis::Jog);
}

// open-astro#2552, the goto sibling: the rate-0 no-op does not supersede a goto either (it takes no claim), so a
// goto that times out after one still stops the stalled axis and keeps its failure.
TEST_CASE("SkyWatcher async - a goto that times out after a rate-0 MoveAxis stops and keeps its failure (#2552)",
          "[skywatcher][async][slewfailure]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    REQUIRE(run_clock_until(clock, [&] { return mount.frames_seen('J') >= 2; }, std::chrono::milliseconds(10000)));
    mount.set_axis_running(2, true, /*speed_mode=*/false);
    move_axis_on_clock(clock, *driver, 1, 0.0);
    std::string message;
    SlewingRead last = SlewingRead::True;
    REQUIRE(run_clock_until(
        clock,
        [&] {
            last = read_slewing(*driver, &message);
            return last != SlewingRead::True;
        },
        kPastSlewTimeout));
    REQUIRE(last == SlewingRead::Threw);
    CHECK(message.find("Slew timed out") != std::string::npos);
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(mount.axis_running(1));
    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);
}

// open-astro#2407 (#2396 AB-7): a goto whose landing stays far off after three re-gotos is a failed slew. It used
// to break out of the refinement and report success. Every landing here stops 1 degree short in Dec and creeps in
// far slower than the refinement looks again.
TEST_CASE("SkyWatcher async - a slew that does not converge after three re-gotos fails and stops",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    mount.land_short_by(2, 10, 1.0, 3600000);
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(target_ra, 40.0));
    // Until the slew ends either way: the old code ended it as a landing (Slewing false, no error).
    std::string message;
    SlewingRead last = SlewingRead::True;
    REQUIRE(run_clock_until(
        clock,
        [&] {
            last = read_slewing(*driver, &message);
            return last != SlewingRead::True;
        },
        std::chrono::milliseconds(60000)));
    REQUIRE(last == SlewingRead::Threw);
    CHECK(message.find("did not converge") != std::string::npos);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_tracking());  // the failure stop leaves the drive off
    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);
}

// open-astro#2407: a stop the board never saw is owed to the next connect. These cases disconnect while the board
// refuses every ":K" on RA (RA first, so the throw ends the pair), then connect again with no link-loss stamp:
// only the owed stop (relink_stop_pending_) can stop the axis, since a connect with no stamp preserves motion.
namespace {

void reconnect_through_refused_disconnect_stop(alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount) {
    mount.reject_ramped_stops(1, 10);
    driver.set_connected(false);
    const bool still_running = mount.axis_running(1) || mount.axis_running(2);
    REQUIRE(still_running);           // the disconnect left an axis running
    mount.reject_ramped_stops(1, 0);  // the board takes stops again
    mount.reject_instant_stop(1, 0);
    driver.set_connected(true);
    REQUIRE(driver.get_connected());
}

}  // namespace

TEST_CASE("SkyWatcher async - a GoTo whose disconnect stop failed is stopped by the next connect",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    // No status poll: its frame would publish RA's GOTO-mode motion as Slewing after the cancel, which the
    // disconnect would also read. Paused, only the sample taken before the cancel says a GoTo was under way.
    sw::set_status_poll_paused_for_testing(*driver, true);
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(5000)));
    mount.reject_instant_stop(1, 10);  // the cancelled body's ":L" fails too, as on a faulted link
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a goto failure stop the board refused is owed to the next connect",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    // No status poll: its frame would publish RA's GOTO-mode motion as Slewing, which the disconnect also reads
    // as a driver motion. Paused, only the refused failure stop says the axis may still run.
    sw::set_status_poll_paused_for_testing(*driver, true);
    mount.reject_start_motion(2, 1);  // the body throws with RA on its way, as in the #2511 case above
    mount.reject_instant_stop(1, 1);  // and the failure stop on RA is refused
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    std::string message;
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver, &message) == SlewingRead::Threw; },
        std::chrono::milliseconds(10000)));
    REQUIRE(mount.axis_running(1));
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(1));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - an AbortSlew the board ignored is owed to the next connect",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    mount.ignore_instant_stops(2, 10);
    driver->abort_slew();
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) == SlewingRead::Threw; }, std::chrono::milliseconds(4000)));
    REQUIRE(mount.axis_running(2));
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a MoveAxis cap stop the board refused is owed to the next connect",
          "[skywatcher][async][moveaxis][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    mount.reject_instant_stop(2, 10);  // the cap's ":L" is refused
    // The status poll and the cap's body (#2651) are the two threads parked on the clock while the jog runs.
    for (int i = 0; i < 59; ++i) {
        REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));
        clock.advance(std::chrono::seconds(1));
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }
    // The cap's body wakes at 60 s and sends its 3 stops, one 250 ms verify round each (#2651).
    for (int i = 0; i < 24; ++i) {
        REQUIRE(clock.wait_for_waiters(kPollerWaiter, kRendezvous));  // the body's end leaves the poll alone
        clock.advance(std::chrono::milliseconds(250));
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    }
    std::string message;
    REQUIRE(read_slewing(*driver, &message) != SlewingRead::True);
    CHECK(message.find("the stop failed") != std::string::npos);
    REQUIRE(mount.axis_running(2));
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a relink stop that fails stays owed to the next connect",
          "[skywatcher][async][moveaxis][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    // The disconnect's ":K" fails (RA first), so the jog is owed a stop; the connect's own stop on Dec fails too.
    mount.reject_ramped_stops(1, 10);
    driver->set_connected(false);
    mount.reject_ramped_stops(1, 0);
    mount.reject_ramped_stops(2, 10);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(mount.axis_running(2));
    // Still owed: a disconnect that fails again, then a connect the board obeys, stops it.
    mount.reject_ramped_stops(2, 0);
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a disconnect whose stops are acked clears an owed stop",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    mount.ignore_instant_stops(2, 10);
    driver->abort_slew();
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) == SlewingRead::Threw; }, std::chrono::milliseconds(4000)));
    driver->set_connected(false);  // both ":K" acked: the jog is stopped, nothing stays owed
    REQUIRE_FALSE(mount.axis_running(2));
    mount.set_axis_running(2, true, /*speed_mode=*/true);  // a handset jog started while disconnected
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(mount.axis_running(2));  // not this driver's motion: left alone
    driver->set_connected(false);
}

// open-astro#2407: the far-miss bound measures RA on the sky. Near the pole an RA-axis error is a small distance
// on the sky: 0.5 degree of RA axis at Dec +88 is about 63 arcsec, a landing, not a 1800 arcsec miss.
TEST_CASE("SkyWatcher async - an RA-axis residual near the pole is measured on the sky",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    mount.land_short_by(1, 10, 0.5, 3600000);  // every RA landing 0.5 degree of axis short, creeping in for an hour
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 2.0 + 24.0, 24.0), 88.0));
    std::string message;
    SlewingRead last = SlewingRead::True;
    REQUIRE(run_clock_until(
        clock,
        [&] {
            last = read_slewing(*driver, &message);
            return last != SlewingRead::True;
        },
        std::chrono::milliseconds(60000)));
    CHECK(last == SlewingRead::False);  // landed: no "did not converge"
    INFO(message);
    driver->abort_slew();
    driver->set_connected(false);
}

// open-astro#2407: a slew body cancelled by a newer command stops the axes itself; when the board refuses that
// ":L", the stop is owed to the next connect. AbortSlew is the newer command here, and its own ":L" on RA is
// refused too, so it throws. The body's failed stop and the disconnect's sample of the motion flags both mark the
// stop owed; the case pins that the next connect pays it.
TEST_CASE("SkyWatcher async - a cancelled slew body whose stop the board refused is owed to the next connect",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    sw::set_status_poll_paused_for_testing(*driver, true);  // no polled Slewing frame to mark the motion instead
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 5.0 + 24.0, 24.0), 20.0));
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(5000)));
    mount.reject_instant_stop(1, 2);  // AbortSlew's ":L" and the cancelled body's ":L" on RA
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);
    REQUIRE(run_clock_until(
        clock, [&] { return read_slewing(*driver) != SlewingRead::True; }, std::chrono::milliseconds(5000)));
    REQUIRE(mount.axis_running(1));
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(1));
    driver->set_connected(false);
}

// open-astro#2407: a failed status read is not a stopped axis. The AbortSlew check reads both axes back; when the
// last read it gets fails, Slewing raises "could not be confirmed" and the stop is owed to the next connect.
TEST_CASE("SkyWatcher async - an AbortSlew whose status reads fail is not confirmed and stays owed",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    // Paused, the poll reads nothing, so the check's own reads are the only ones and the link latch (three failed
    // exchanges in a row) is not reached: fail, good, fail.
    sw::set_status_poll_paused_for_testing(*driver, true);
    mount.ignore_instant_stops(2, 10);
    driver->abort_slew();
    const auto check_poll = [&](bool fail_read) {
        if (fail_read) mount.fail_status_replies(1, 1);
        REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));  // the paused poll and the check
        clock.advance(std::chrono::milliseconds(250));
        REQUIRE(clock.wait_for_woken_settled(kRendezvous));
    };
    check_poll(true);
    check_poll(false);
    check_poll(true);
    std::string message;
    REQUIRE(read_slewing(*driver, &message) != SlewingRead::True);
    CHECK(message.find("could not be confirmed (status reads failed)") != std::string::npos);
    REQUIRE(mount.axis_running(2));
    sw::set_status_poll_paused_for_testing(*driver, false);
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

// open-astro#2407: Unpark during a Park cancels the park body and stops the axes itself. When its ":K" fails, the
// stop is owed to the next connect. The body's own ":L" is acked but ignored here, so only Unpark's failed stop
// can mark it, and the paused poll publishes no Slewing frame that the disconnect would also read.
TEST_CASE("SkyWatcher async - an Unpark stop the board refused is owed to the next connect",
          "[skywatcher][async][park][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 45.0);  // away from the park position, so the park slew moves RA
    sw::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE_NOTHROW(driver->park());
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(5000)));
    mount.ignore_instant_stops(1, 10);  // the cancelled body's ":L": acked, RA runs on
    mount.reject_ramped_stops(1, 10);   // Unpark's ":K" throws
    driver->unpark();
    REQUIRE(mount.axis_running(1));
    reconnect_through_refused_disconnect_stop(*driver, mount);
    CHECK_FALSE(mount.axis_running(1));
    driver->set_connected(false);
}

// open-astro#2407: every re-goto checks the altitude floor, as the first leg does. The first leg lands 1 degree
// short in Dec; a latitude write while it lands puts the target below the floor, so the re-goto is refused.
TEST_CASE("SkyWatcher async - a re-goto whose target is below the altitude floor is refused",
          "[skywatcher][async][slewfailure][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    alpacacore::util::MotionLimits limits;
    limits.min_altitude_deg = 0.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 51.477833, -0.0015, 42.0, {}, limits, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);
    mount.land_short_by(2, 10, 1.0, 3600000);
    const double lst = driver->get_sidereal_time();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(std::fmod(lst - 2.0 + 24.0, 24.0), 40.0));
    driver->set_site_latitude(-60.0);  // Dec +40 two hours west is now below the horizon
    std::string message;
    SlewingRead last = SlewingRead::True;
    REQUIRE(run_clock_until(
        clock,
        [&] {
            last = read_slewing(*driver, &message);
            return last != SlewingRead::True;
        },
        std::chrono::milliseconds(60000)));
    REQUIRE(last == SlewingRead::Threw);
    CHECK(message.find("Slew re-goto") != std::string::npos);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);
}

namespace {

// open-astro#2604: which Park or FindHome a case starts. The count-frame FindHome is a goto to the power-on index;
// a board with home indexers (the Wave) runs AutoHome instead.
enum class HomingLeg : std::uint8_t { Park, CountFrameHome, AutoHome };

const char* leg_name(HomingLeg leg) {
    switch (leg) {
        case HomingLeg::Park:
            return "Park";
        case HomingLeg::CountFrameHome:
            return "FindHome (count frame)";
        case HomingLeg::AutoHome:
            return "FindHome (AutoHome)";
    }
    return "?";
}

}  // namespace

// open-astro#2604 (the #2586 decision): AbortSlew cancels a Park or FindHome body and does not join it. Once AbortSlew
// has returned, it owns the end of that operation: Slewing reads false, SiteLongitude is accepted, and the cancelled
// body sends nothing more to the board while it winds down. The wake seam holds the cancelled body as its sleep
// returns, before it takes a driver lock, so the reads and the write land inside that window every run.
TEST_CASE("SkyWatcher async - AbortSlew ends a Park or FindHome before its cancelled body winds down (#2604)",
          "[skywatcher][async][abort][goto-safety]") {
    const HomingLeg leg = GENERATE(HomingLeg::Park, HomingLeg::CountFrameHome, HomingLeg::AutoHome);
    INFO(leg_name(leg));
    alpacacore::test::Latch held;  // declared first: it outlives the driver the hook below is set on
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(leg == HomingLeg::CountFrameHome ? alpacacore::test::FakeMountProfile::eqm35_pro()
                                                               : alpacacore::test::FakeMountProfile::wave_100i(),
                              clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 45.0);                       // away from home and park, so the operation moves RA
    sw::set_status_poll_paused_for_testing(*driver, true);  // no polled Slewing frame answers for the flags
    const auto body_ended = std::make_shared<std::atomic<bool>>(false);  // the body thread never outlives it
    sw::set_slew_spawn_for_testing(*driver, [body_ended](std::function<void()> body) {
        return std::thread([body_ended, body = std::move(body)] {
            body();
            body_ended->store(true);
        });
    });
    if (leg == HomingLeg::Park) {
        REQUIRE_NOTHROW(driver->park());
    } else {
        REQUIRE_NOTHROW(driver->find_home());
    }
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(10000)));
    sw::set_stopped_body_wake_hook_for_testing(*driver, [&held] { held.block(); });

    REQUIRE_NOTHROW(driver->abort_slew());
    REQUIRE(held.wait_entered());  // the cancelled body woke and is held before it takes a driver lock
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_NOTHROW(driver->set_site_longitude(10.0));
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(body_ended->load());  // all of the above ran while the body was still winding down

    const std::size_t motion_before = mount.motion_log().size();
    held.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!body_ended->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(body_ended->load());
    CHECK(mount.motion_log().size() == motion_before);  // the body's tail sent no stop and no motion
    CHECK(read_slewing(*driver) == SlewingRead::False);
    sw::set_stopped_body_wake_hook_for_testing(*driver, {});
    driver->set_connected(false);
}

// open-astro#2635: the goto form of the case above. AbortSlew stops both axes and ends the goto before the cancelled
// body unwinds; the body's failure path used to send both ":L" stops again after AbortSlew had returned. The two
// Target forms call the Coordinates ones, so the case runs SlewToCoordinatesAsync and SlewToTargetAsync.
TEST_CASE("SkyWatcher async - a goto cancelled by AbortSlew sends no stop as its body unwinds (#2635)",
          "[skywatcher][async][abort][goto-safety]") {
    const bool to_target = GENERATE(false, true);
    INFO((to_target ? "SlewToTargetAsync" : "SlewToCoordinatesAsync"));
    alpacacore::test::Latch held;  // declared first: it outlives the driver the hook below is set on
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->set_tracking(true);
    sw::set_status_poll_paused_for_testing(*driver, true);  // no polled Slewing frame answers for the flags
    const auto body_ended = std::make_shared<std::atomic<bool>>(false);  // the body thread never outlives it
    sw::set_slew_spawn_for_testing(*driver, [body_ended](std::function<void()> body) {
        return std::thread([body_ended, body = std::move(body)] {
            body();
            body_ended->store(true);
        });
    });
    const double ra = std::fmod(driver->get_sidereal_time() - 2.0 + 24.0, 24.0);
    if (to_target) {
        driver->set_target_right_ascension(ra);
        driver->set_target_declination(40.0);
        REQUIRE_NOTHROW(driver->slew_to_target_async());
    } else {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(ra, 40.0));
    }
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(10000)));
    // The goto body polls the board between sleeps: AbortSlew must find it in a sleep, so the wake seam holds it.
    REQUIRE(clock.wait_for_waiters(kPollerWaiter + 1, kRendezvous));
    sw::set_stopped_body_wake_hook_for_testing(*driver, [&held] { held.block(); });

    const std::size_t motion_before_abort = mount.motion_log().size();
    REQUIRE_NOTHROW(driver->abort_slew());
    REQUIRE(held.wait_entered());  // the cancelled body woke and is held before it takes a driver lock
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(body_ended->load());
    CHECK(mount.motion_log().size() > motion_before_abort);  // AbortSlew's own stops

    const std::size_t motion_before = mount.motion_log().size();
    held.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!body_ended->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(body_ended->load());
    CHECK(mount.motion_log().size() == motion_before);  // the body's tail sent no stop and no motion
    CHECK(read_slewing(*driver) == SlewingRead::False);
    sw::set_stopped_body_wake_hook_for_testing(*driver, {});
    driver->set_connected(false);
}

// open-astro#2604: a body cancelled in its landing settle wait reads nothing more from the board. The Park lands, its
// body reads RA's position (`:j`) and sleeps the settle interval; AbortSlew cancels it there, and the wake seam holds
// it until the reads AbortSlew makes itself are done. Released, the body ends with no status (`:f`) or position
// (`:j`) read.
TEST_CASE("SkyWatcher async - a body cancelled in its landing settle wait reads nothing more (#2604)",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::Latch held;  // declared first: it outlives the driver the hook below is set on
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 45.0);
    sw::set_status_poll_paused_for_testing(*driver, true);               // only the body reads the board
    const auto body_ended = std::make_shared<std::atomic<bool>>(false);  // the body thread never outlives it
    sw::set_slew_spawn_for_testing(*driver, [body_ended](std::function<void()> body) {
        return std::thread([body_ended, body = std::move(body)] {
            body();
            body_ended->store(true);
        });
    });
    REQUIRE_NOTHROW(driver->park());
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(10000)));
    REQUIRE(run_clock_until(
        clock, [&] { return !mount.axis_running(1) && !mount.axis_running(2); }, std::chrono::milliseconds(60000)));
    // The wait loop reads only `:f`; the settle wait starts with a `:j` and then sleeps.
    const int positions_at_stop = mount.frames_seen('j');
    REQUIRE(run_clock_until(
        clock, [&] { return mount.frames_seen('j') > positions_at_stop; }, std::chrono::milliseconds(5000)));
    REQUIRE(read_slewing(*driver) == SlewingRead::True);  // still parking: the body is in the settle sleep
    sw::set_stopped_body_wake_hook_for_testing(*driver, [&held] { held.block(); });

    REQUIRE_NOTHROW(driver->abort_slew());
    REQUIRE(held.wait_entered());
    const int status_reads = mount.frames_seen('f');
    const int position_reads = mount.frames_seen('j');
    held.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!body_ended->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(body_ended->load());
    CHECK(mount.frames_seen('f') == status_reads);
    CHECK(mount.frames_seen('j') == position_reads);
    CHECK_FALSE(driver->get_at_park());
    sw::set_stopped_body_wake_hook_for_testing(*driver, {});
    driver->set_connected(false);
}

// open-astro#2604: AbortSlew ends a Park only after its own stops were answered. When its RA ":L" is refused,
// AbortSlew throws and leaves the Park to its cancelled body, which still stops both axes as it winds down.
TEST_CASE("SkyWatcher async - a Park whose AbortSlew stop failed is still stopped by its body (#2604)",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 45.0);
    sw::set_status_poll_paused_for_testing(*driver, true);
    const auto body_ended = std::make_shared<std::atomic<bool>>(false);  // the body thread never outlives it
    sw::set_slew_spawn_for_testing(*driver, [body_ended](std::function<void()> body) {
        return std::thread([body_ended, body = std::move(body)] {
            body();
            body_ended->store(true);
        });
    });
    REQUIRE_NOTHROW(driver->park());
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(10000)));
    const int ra_stops = mount.stop_count(1);
    const int dec_stops = mount.stop_count(2);
    mount.reject_instant_stop(1, 1);  // AbortSlew's RA ":L" only
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!body_ended->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(body_ended->load());
    CHECK(mount.stop_count(1) > ra_stops);   // the body's RA stop
    CHECK(mount.stop_count(2) > dec_stops);  // and its Dec stop, which AbortSlew never reached
    CHECK_FALSE(mount.axis_running(1));
    CHECK_FALSE(mount.axis_running(2));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    CHECK_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

// open-astro#2604: the other side of the case above. A Park that runs, with no AbortSlew, still refuses SiteLongitude
// (open-astro#2407: it sets the sidereal time the park slew aims by), and AbortSlew lets it through again.
TEST_CASE("SkyWatcher async - SiteLongitude is refused while a Park runs", "[skywatcher][async][park][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    mount.jump_axis_degrees(1, 45.0);
    sw::set_status_poll_paused_for_testing(*driver, true);
    REQUIRE_NOTHROW(driver->park());
    REQUIRE(run_clock_until(clock, [&] { return mount.axis_running(1); }, std::chrono::milliseconds(10000)));
    alpacacore::test::require_alpaca_error([&] { driver->set_site_longitude(10.0); },
                                           alpacacore::AlpacaError::InvalidOperation);
    CHECK(driver->get_site_longitude() == Catch::Approx(-104.9903));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK_NOTHROW(driver->set_site_longitude(10.0));
    driver->set_connected(false);
}

// open-astro#2407: the AbortSlew stop check is what proves an acked ":L" stopped the axes. When that check cannot
// start (the OS refuses its thread), nothing confirms the stop, so the next Slewing read must raise it instead of
// reading as a stopped mount. Dec's ":L" is acked and ignored here, so the axis runs on.
TEST_CASE("SkyWatcher async - an AbortSlew whose stop check does not start raises through Slewing",
          "[skywatcher][async][abort][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    driver->move_axis(1, 1.0);
    REQUIRE(mount.axis_running(2));
    mount.ignore_instant_stops(2, 10);
    sw::set_abort_verify_spawn_for_testing(*driver, [](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    REQUIRE_NOTHROW(driver->abort_slew());
    std::string message;
    CHECK(read_slewing(*driver, &message) == SlewingRead::Threw);
    CHECK(message.find("AbortSlew: the stop could not be confirmed") != std::string::npos);
    CHECK(mount.axis_running(2));  // the fake still runs: only the check would have resent the stop
    driver->set_connected(false);
}

// open-astro#2407 (the issue's Decisions, 2026-11-24): a Sync more than util::kMaxSyncOffsetDegrees from where
// the mount points is refused (InvalidValue), and writes nothing. A Sky-Watcher board has no sky reference at power-up,
// so the first Sync after each Connect is exempt: it sets the reference.
namespace {

bool sync_refused_invalid_value(alpacacore::TelescopeDriver& driver, double ra, double dec) {
    try {
        driver.sync_to_coordinates(ra, dec);
    } catch (const alpacacore::AlpacaException& ex) {
        return ex.error_code() == alpacacore::AlpacaError::InvalidValue;
    }
    return false;
}

}  // namespace

TEST_CASE("SkyWatcher sync - a Sync more than 5 degrees off after the first is refused and writes nothing",
          "[skywatcher][sync][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    // The first Sync of the session is far from the power-up frame, and is taken.
    const double first_ra = std::fmod(driver->get_right_ascension() + 3.0, 24.0);
    REQUIRE_NOTHROW(driver->sync_to_coordinates(first_ra, 20.0));
    const double ra = driver->get_right_ascension();
    const double dec = driver->get_declination();
    const int writes_before = mount.frames_seen('E');
    const double axis1_before = mount.axis_degrees(1);
    const double axis2_before = mount.axis_degrees(2);
    CHECK(sync_refused_invalid_value(*driver, ra, dec + 10.0));
    CHECK(mount.frames_seen('E') == writes_before);  // no position write
    CHECK(mount.axis_degrees(1) == axis1_before);
    CHECK(mount.axis_degrees(2) == axis2_before);
    CHECK(std::abs(driver->get_declination() - dec) < 0.01);
    // A Sync within the bound is still taken.
    REQUIRE_NOTHROW(driver->sync_to_coordinates(ra, dec + 2.0));
    CHECK(mount.frames_seen('E') > writes_before);
    CHECK(std::abs(driver->get_declination() - (dec + 2.0)) < 0.01);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher sync - each Connect exempts its first Sync again", "[skywatcher][sync][goto-safety]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount, clock);
    REQUIRE_NOTHROW(driver->sync_to_coordinates(std::fmod(driver->get_right_ascension() + 3.0, 24.0), 20.0));
    driver->set_connected(false);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // A new session: its first Sync sets the reference however far off.
    const double ra = std::fmod(driver->get_right_ascension() + 6.0, 24.0);
    REQUIRE_NOTHROW(driver->sync_to_coordinates(ra, -10.0));
    CHECK(std::abs(driver->get_declination() - (-10.0)) < 0.01);
    // The second Sync of that session is bounded.
    const int writes_before = mount.frames_seen('E');
    CHECK(sync_refused_invalid_value(*driver, ra, 10.0));
    CHECK(mount.frames_seen('E') == writes_before);
    driver->set_connected(false);
}

#endif  // _WIN32
