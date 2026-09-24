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
#include <alpacacore/util/host_clock.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace {
// Same shape as the unit file's helper: the call must throw an
// AlpacaException carrying exactly this error code.
void expect_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == expected_code);
    }
}
// open-astro#395: pins the host-discipline probe for one case and restores
// the real adjtimex read afterwards, whatever the case does.
struct ProbeGuard {
    explicit ProbeGuard(bool disciplined) {
        alpacacore::vendor::skywatcher::detail::set_host_synchronized_probe([disciplined] { return disciplined; });
    }
    explicit ProbeGuard(std::function<bool()> probe, std::chrono::milliseconds resample_interval)
        : previous_interval_(alpacacore::vendor::skywatcher::detail::host_discipline_resample_interval()) {
        alpacacore::vendor::skywatcher::detail::set_host_synchronized_probe(std::move(probe));
        alpacacore::vendor::skywatcher::detail::set_host_discipline_resample_interval(resample_interval);
    }
    ~ProbeGuard() {
        // Restore the interval BEFORE the probe: from here on nothing may
        // call the lambda this guard installed, whatever it captured.
        alpacacore::vendor::skywatcher::detail::set_host_discipline_resample_interval(previous_interval_);
        alpacacore::vendor::skywatcher::detail::set_host_synchronized_probe(nullptr);
    }
    std::chrono::milliseconds previous_interval_{30000};
};
}  // namespace

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

// open-astro#559: IsPulseGuiding followed a deadline stamped at dispatch
// (duration + 1000 ms) rather than the pulse task's actual stop, so it stayed
// true about a second after the axis had stopped, and on a stalled stop it
// could clear while the axis was still moving. It must track the real stop.
TEST_CASE("SkyWatcher async - IsPulseGuiding clears when the pulse's axis stops, not a second later (#559)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    driver->pulse_guide(0, 500);  // North
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 3000));
    const auto stopped_at = std::chrono::steady_clock::now();
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 3000));
    const auto lag_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopped_at).count();
    // The bound is loose on purpose: the claim is not that the flag clears
    // instantly, only that it does not carry a fixed extra second.
    CHECK(lag_ms < 300);
    driver->set_connected(false);
}

// open-astro#559 (thread): a new pulse sets the flag, then reaps the running
// one, and the reaped task's cancel path cleared the flag the NEW pulse had
// just set -- so an autoguider's back-to-back pulses read IsPulseGuiding
// false while the second pulse was still moving the axis.
TEST_CASE("SkyWatcher async - a pulse that supersedes another still reports IsPulseGuiding (#559)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    driver->pulse_guide(0, 2000);  // North
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    driver->pulse_guide(0, 2000);  // supersedes the first mid-pulse
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(mount.axis_running(2));
    CHECK(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 6000));
    driver->set_connected(false);
}

// open-astro#559 (review): once IsPulseGuiding reads false the pulse must also
// have released its axis. A client that waits for the property and then
// writes DeclinationRate is following the ASCOM contract; if the pulse still
// owned the Dec axis, the write took the busy-axis deferral and waited for a
// restore the pulse had already run, so the offset was stranded.
TEST_CASE("SkyWatcher async - a DeclinationRate write right after IsPulseGuiding clears is applied (#559)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    driver->pulse_guide(0, 500);  // North
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 3000));
    REQUIRE_FALSE(mount.axis_running(2));
    driver->set_declination_rate(10.0);  // arcsec/s, continuous (above the floor)
    CHECK(wait_until([&] { return mount.axis_running(2); }, 3000));
    driver->set_declination_rate(0.0);
    driver->set_connected(false);
}

// open-astro#620 (regression, fixed below): pulse_guide() USED TO HAVE one
// pulse task slot shared by both axes. reap_pulse_task() cancelled+joined
// whatever pulse task was running regardless of axis, and a cancelled task's
// cancel path deliberately does not touch the hardware (the reaper is
// supposed to stop or re-command the axes itself -- see the #559 comment on
// the pulse task lambda). Every other reaper (goto/park/home/abort/MoveAxis/
// disconnect) re-commands or stops BOTH axes, but pulse_guide() only
// dispatches its OWN axis: an RA pulse arriving mid-Dec pulse reaped the Dec
// task and only commanded RA, leaving Dec running at guide rate with nothing
// left to stop it. Pulse tasks are now per-axis (pulse_task_thread_[2]) and
// reap_pulse_task(axis) reaps only its own axis's task.
TEST_CASE("SkyWatcher async - an RA pulse does not leave a running Dec pulse's axis turning (#620)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch (a2 > 0)

    driver->pulse_guide(0, 3000);  // Dec North, 3 s
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    driver->pulse_guide(2, 300);  // RA East, 300 ms -- concurrent, different axis
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    INFO("IsPulseGuiding=" << driver->get_is_pulse_guiding() << " Slewing=" << driver->get_slewing());
    CHECK_FALSE(mount.axis_running(2));
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// open-astro#620 review W3: the mirror direction of the case above has no
// coverage without this -- a Dec pulse arriving mid-RA-pulse must not strand
// RA at the (in-place) guide-rate step period. This is the harder direction
// to catch: a stuck RA axis is still `axis_running(1) == true` (it keeps
// turning, just at the wrong rate), so only a step-period check can see it.
TEST_CASE("SkyWatcher async - a Dec pulse does not leave a running RA pulse's axis at the guide rate (#620)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch, as the case above
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);

    driver->pulse_guide(2, 3000);  // RA East, 3 s -- default guide rate, in-place branch
    REQUIRE(wait_until([&] { return mount.step_period(1) != sidereal_preset; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    driver->pulse_guide(0, 300);  // Dec North, 300 ms -- concurrent, different axis
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    INFO("IsPulseGuiding=" << driver->get_is_pulse_guiding() << " step_period(1)=" << mount.step_period(1)
                           << " sidereal=" << sidereal_preset);
    CHECK(mount.step_period(1) == sidereal_preset);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

// open-astro#620: two concurrent callers, one per axis, repeated -- both must
// always end with their own axis released (RA back to the sidereal tracking
// PERIOD, not merely "running" -- open-astro#620 review W2: RA stuck at the
// East pulse rate is still `axis_running(1) == true`, so that check alone
// cannot see the RA-stranded-at-guide-rate failure this case exists to
// catch -- and Dec stopped), never leaving the other's pulse cancelled
// without a replacement command.
TEST_CASE("SkyWatcher async - concurrent RA and Dec PulseGuide callers both end with their axes released (#620)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);

    for (int round = 0; round < 10; ++round) {
        std::thread ra_caller([&] { driver->pulse_guide(2, 300); });
        std::thread dec_caller([&] { driver->pulse_guide(0, 300); });
        ra_caller.join();
        dec_caller.join();
        REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 4000));
        REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 3000));
        REQUIRE(wait_until([&] { return mount.step_period(1) == sidereal_preset; }, 3000));
        CHECK(mount.axis_running(1));  // RA restored to tracking, not stranded stopped
    }
    driver->set_connected(false);
}

// open-astro#306: the CONTROL for the hardware measurement on that issue.
// An EQ-AL55i Pro delivers 99.0% of a 5000 ms Dec pulse but only 47.6% of a
// 500 ms one, which fits a fixed per-start cost rather than a rate error.
// This fake has no start ramp -- Axis::advance() moves at rate_counts for
// the whole time the axis is running -- so a correct driver on a perfect
// board must deliver the same fraction at EVERY duration. That is what makes
// the hardware numbers attributable to the board rather than to the driver's
// dispatch, and it is where a compensation would be pinned: when the fake
// learns a start ramp, this case is what says the driver corrects for it.
// Tolerance is asymmetric: at least (rate x duration) - 1 count, at most
// + 3. A flat percentage band admits exactly one integer count value at
// 500 ms, pinning real axis-on time to a ~40 ms window and turning ordinary
// scheduling jitter into a flaky failure -- the first fix for that (a flat
// +-1 count) was itself still spent entirely on overshoot, since undershoot
// is structurally impossible here: task_wait_for(remaining) never returns
// early and the fake starts integrating at ":J", before the driver's own
// dispatch cost is paid, so delivered counts can only be AT LEAST
// trunc(rate x duration) (Axis::advance()'s remainder carry keeps that
// floor under one count at any duration) and can exceed it by however long
// task_wait_for's wakeup, the ":K" round trip and stop_axis()'s mutex
// acquisition take. A flat +-1 band therefore left ~70-90 ms of real budget
// entirely on the overshoot side while still failing on a few ms of it --
// four review rounds on open-astro#603 measured the same ~80 ms figure
// independently. -1/+3 keeps the tight lower bound that actually catches
// the regression this branch fixes (a ~21% shortfall does not survive -1)
// while giving overshoot the room the timing actually needs.
TEST_CASE("SkyWatcher async - Dec pulse delivery is flat across durations on a board with no start cost (#306)",
          "[skywatcher][async][pulseguide]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch, as the cases above

    const double rate_deg_per_sec = driver->get_guide_rate().dec;
    REQUIRE(rate_deg_per_sec > 0.0);

    for (int duration : {500, 1000, 2000, 5000}) {
        for (int direction : {0, 1}) {  // North, South -- alternating, so the axis stays put
            const double before = mount.axis_degrees(2);
            driver->pulse_guide(direction, duration);
            // Both edges, not just the trailing one: a bare "not running" wait
            // is satisfied at t=0, before the task thread commands motion.
            REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
            REQUIRE(wait_until([&] { return !mount.axis_running(2); }, duration + 5000));
            REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, duration + 5000));

            const double moved_deg = std::abs(mount.axis_degrees(2) - before);
            const double expected_deg = rate_deg_per_sec * duration / 1000.0;
            const double counts_per_deg = mount.kCpr / 360.0;
            const double moved_counts = moved_deg * counts_per_deg;
            const double expected_counts = expected_deg * counts_per_deg;
            INFO("duration " << duration << " ms, direction " << direction << ": moved " << moved_counts
                             << " counts of " << expected_counts << " expected (" << moved_deg * 3600.0 << " arcsec of "
                             << expected_deg * 3600.0 << ")");
            // -1/+3 counts of rate x duration: undershoot is impossible (see
            // the header comment), so the lower bound stays at the fake's own
            // quantisation floor -- tight enough to catch the ~21% shortfall
            // this branch fixes -- while the upper bound absorbs the real
            // overshoot budget (task_wait_for wakeup + the ":K" round trip +
            // stop_axis()'s mutex) instead of spending a flat +-1 band
            // entirely on one side of a symmetric check.
            CHECK(moved_counts >= expected_counts - 1.0);
            CHECK(moved_counts <= expected_counts + 3.0);
        }
    }
    driver->set_connected(false);
}

// open-astro#306, hardware row: the case above proves the fake has no
// per-start cost using the DEFAULT profile (Wave 100i, 4,147,200 cpr,
// 14 MHz timer) -- geometry that has never belonged to the board the #306
// hardware numbers (48% at 500 ms, 99% at 5 s) were measured on. This repeats
// it against FakeMountProfile::eq_al55i(): 4,032,000 cpr RA / 3,600,000 Dec
// (the only profile here where the two axes differ), 16 MHz timer, mount
// code 0x09. Different cpr changes how many counts a given arcsecond of
// motion quantises to, so a rounding-driven bug in Axis::advance() could
// pass on one profile's numbers and fail on the other's -- this closes that
// gap rather than trusting the default profile to stand in for every board.
TEST_CASE("SkyWatcher async - Dec pulse delivery is flat across durations on the EQ-AL55i Pro's own geometry (#306)",
          "[skywatcher][async][pulseguide][al55i]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eq_al55i());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch, as the cases above

    const double rate_deg_per_sec = driver->get_guide_rate().dec;
    REQUIRE(rate_deg_per_sec > 0.0);

    for (int duration : {500, 1000, 2000, 5000}) {
        for (int direction : {0, 1}) {  // North, South -- alternating, so the axis stays put
            const double before = mount.axis_degrees(2);
            driver->pulse_guide(direction, duration);
            REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
            REQUIRE(wait_until([&] { return !mount.axis_running(2); }, duration + 5000));
            REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, duration + 5000));

            const double moved_deg = std::abs(mount.axis_degrees(2) - before);
            const double expected_deg = rate_deg_per_sec * duration / 1000.0;
            // Dec-axis cpr, NOT mount.kCpr (that's RA's) -- this is the one
            // profile here where the two differ.
            const double counts_per_deg = mount.kCprDec / 360.0;
            const double moved_counts = moved_deg * counts_per_deg;
            const double expected_counts = expected_deg * counts_per_deg;
            INFO("duration " << duration << " ms, direction " << direction << ": moved " << moved_counts
                             << " counts of " << expected_counts << " expected (" << moved_deg * 3600.0 << " arcsec of "
                             << expected_deg * 3600.0 << ")");
            // Same -1/+3 count rationale as the default-profile case above.
            CHECK(moved_counts >= expected_counts - 1.0);
            CHECK(moved_counts <= expected_counts + 3.0);
        }
    }
    driver->set_connected(false);
}

// open-astro#306: the EQ-AL55i Pro's owner read ":s1"/":s2" through
// CommandString(Raw=true) on 2026-09-22 and got "=000000" on both axes, twice
// -- a real zero, not the "!0" the fixture used to send for every board whose
// steps-per-worm is 0. A board that answers 0 and a board that rejects ":s"
// are different captures; the fake has to be able to reproduce both.
TEST_CASE("SkyWatcher async - ':s' replies match each captured board (#306)", "[skywatcher][async][al55i]") {
    SECTION("EQ-AL55i Pro answers zero on both axes") {
        FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eq_al55i());
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        CHECK(driver->command_string(":s1", true) == "=000000");
        CHECK(driver->command_string(":s2", true) == "=000000");
        driver->set_connected(false);
    }
    SECTION("EQM-35 Pro answers 68266") {
        FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        CHECK(driver->command_string(":s1", true) == "=AA0A01");  // 68266 = 0x010AAA
        driver->set_connected(false);
    }
    SECTION("Wave 100i rejects the command") {
        FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        CHECK(driver->command_string(":s1", true) == "!0");
        driver->set_connected(false);
    }
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

namespace {
// Counts ERROR-level "telescope" log lines mentioning the #547 watchdog, so
// a case can assert it fired (or didn't) without depending on log text
// beyond the one word that identifies it.
struct WatchdogLogGuard {
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    std::atomic<int> count{0};
    WatchdogLogGuard() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Error && component == "telescope" &&
                    message.find("watchdog") != std::string_view::npos) {
                    ++count;
                }
            });
    }
    ~WatchdogLogGuard() { alpacacore::logging::set_log_sink(previous); }
};
}  // namespace

// open-astro#547: the client-silence motion watchdog. Written RED FIRST
// against unmodified code -- before TelescopeDriver grew
// note_client_activity()/stop_motion_if_client_silent(), this case (and the
// three below it) failed to COMPILE (no such member on TelescopeDriver),
// which is the compile-time form of red for a seam that does not exist yet.
TEST_CASE("SkyWatcher async - client-silence watchdog stops a moving axis after the interval",
          "[skywatcher][async][watchdog]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    WatchdogLogGuard log_guard;

    const auto t0 = std::chrono::steady_clock::now();
    driver->note_client_activity(t0);
    driver->move_axis(0, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    REQUIRE(driver->get_slewing());
    const int stops_before = mount.stop_count(1);

    REQUIRE(driver->stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));

    REQUIRE(wait_until([&] { return !mount.axis_running(1); }, 5000));
    CHECK(mount.stop_count(1) > stops_before);
    REQUIRE_FALSE(driver->get_slewing());
    CHECK(log_guard.count.load() == 1);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - client-silence watchdog never trips while a client keeps polling",
          "[skywatcher][async][watchdog]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    WatchdogLogGuard log_guard;

    const auto t0 = std::chrono::steady_clock::now();
    driver->note_client_activity(t0);
    driver->move_axis(0, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // A client polling every second (well inside the 30 s interval) must
    // never see the watchdog fire, however long the slew runs.
    for (int i = 1; i <= 45; ++i) {
        const auto tick = t0 + std::chrono::seconds(i);
        driver->note_client_activity(tick);
        REQUIRE_FALSE(driver->stop_motion_if_client_silent(tick, std::chrono::seconds(30)));
    }
    CHECK(mount.axis_running(1));
    CHECK(driver->get_slewing());
    CHECK(log_guard.count.load() == 0);

    driver->move_axis(0, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - client-silence watchdog never trips a tracking-only mount",
          "[skywatcher][async][watchdog]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    WatchdogLogGuard log_guard;

    const auto t0 = std::chrono::steady_clock::now();
    driver->note_client_activity(t0);
    REQUIRE_FALSE(driver->get_slewing());
    const int stops_before = mount.stop_count(1);

    // Ten minutes of silence on a mount that is only tracking, never armed
    // because Slewing was never true.
    REQUIRE_FALSE(driver->stop_motion_if_client_silent(t0 + std::chrono::seconds(600), std::chrono::seconds(30)));

    CHECK(driver->get_tracking());
    CHECK(mount.stop_count(1) == stops_before);
    CHECK(log_guard.count.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - client-silence watchdog never trips on a pulse guide", "[skywatcher][async][watchdog]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    WatchdogLogGuard log_guard;

    const auto t0 = std::chrono::steady_clock::now();
    driver->note_client_activity(t0);
    driver->pulse_guide(0, 3000);  // North, 3 s
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE_FALSE(driver->get_slewing());

    REQUIRE_FALSE(driver->stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));

    CHECK(log_guard.count.load() == 0);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - client-silence watchdog is disabled by a non-positive interval",
          "[skywatcher][async][watchdog]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    WatchdogLogGuard log_guard;

    const auto t0 = std::chrono::steady_clock::now();
    driver->note_client_activity(t0);
    driver->move_axis(0, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    REQUIRE_FALSE(driver->stop_motion_if_client_silent(t0 + std::chrono::seconds(600), std::chrono::seconds(0)));
    CHECK(mount.axis_running(1));
    CHECK(log_guard.count.load() == 0);

    driver->move_axis(0, 0.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    driver->set_connected(false);
}

TEST_CASE(
    "SkyWatcher async - independent MoveAxis stops on both axes do not strand Slewing "
    "or block the RA tracking restore",
    "[skywatcher][async]") {
    // Regression (found during EQM-35 Pro hardware bring-up, 2026-09-06), fixed in two
    // steps:
    //
    // (1) reap_stop_task() used to cancel+join a SINGLE stop-completion thread shared by
    //     both axes. Stopping axis 1 while axis 0's stop task was still polling a
    //     ramping mount (CCDciel issues MoveAxis stop pairs ~44ms apart on button
    //     release -- see .github/instructions/skywatcher.instructions.md) cancelled the RA task before it reached
    //     manual_axis_slewing_[0] = false, stranding Slewing true FOREVER
    //     (get_hardware_slewing_locked() ORs both axes' flags) -- exactly the hardware
    //     symptom. Fixed: each axis now has its own stop-task thread and cancel flag.
    //
    // (2) That fix alone was not sufficient: the RA stop task's tracking-restore tail
    //     guarded itself with `motion_generation_ == stop_task_generation`, a counter
    //     bumped by EVERY motion command on EITHER axis. Dispatching the Dec stop
    //     bumped it for a reason unrelated to RA, so the RA tail read a mismatch and
    //     silently skipped restoring RA's tracking, even though Slewing correctly
    //     cleared. Fixed by applying the same `same_axis_owner` idiom already used by
    //     the duty-cycle worker: only treat a generation mismatch as a real
    //     supersession when something that can actually own THIS axis (goto/park/home/
    //     pulse-guide on this channel) is responsible for it.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);  // long enough for the second stop to race it
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    driver->move_axis(0, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    driver->move_axis(1, 2.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));

    driver->move_axis(0, 0.0);  // RA stop task starts polling; ramp takes 800ms
    driver->move_axis(1, 0.0);  // Dec stop dispatched almost immediately after

    // Under the old shared-thread bug this hung until the wait_until timeout
    // (Slewing stuck true forever); it must now clear promptly.
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // Under the (now-fixed) generation-counter bug, Slewing cleared correctly but
    // the RA axis stayed stopped on the mount despite Tracking still reading true.
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 5000));
    CHECK(driver->get_tracking());

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

TEST_CASE("SkyWatcher async - AbortSlew during the dispatch stop-wait kills the goto", "[skywatcher][async]") {
    // PR #216 review race: a goto dispatch stop-waits a ramping axis with the
    // mutex released; an AbortSlew landing in that window must supersede the
    // dispatch — the old code re-commanded the aborted goto afterwards.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);  // wide unlock window during dispatch
    auto driver = connected_driver(mount);
    driver->set_tracking(true);  // RA axis moving: dispatch must stop-wait it

    double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 4.0 + 24.0, 24.0), 30.0);
    // Abort while the dispatch is still ramping the RA axis down.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->abort_slew();
    REQUIRE_FALSE(driver->get_slewing());
    int ra_starts = mount.start_count(1);
    int dec_starts = mount.start_count(2);

    // The superseded dispatch must never re-command the goto — not even a
    // brief start-then-stop burst: NO ":J" may reach the controller after
    // AbortSlew returned (PR #216 round-2 finding).
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    REQUIRE(mount.start_count(1) == ra_starts);
    REQUIRE(mount.start_count(2) == dec_starts);
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(mount.axis_running(1));
    REQUIRE_FALSE(mount.axis_running(2));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - NaN slew is rejected before any motion command (#574)", "[skywatcher][async]") {
    // #574: validate_ra_dec used `x < min || x > max`, which is false for
    // NaN, so a NaN declination reached the goto dispatch and started
    // motion. Pin that it is rejected before slewing_cached_ is set and
    // before any ":J" start command reaches the controller.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    int ra_starts_before = mount.start_count(1);
    int dec_starts_before = mount.start_count(2);

    expect_alpaca_error([&] { driver->slew_to_coordinates_async(std::nan(""), 0.0); },
                        alpacacore::AlpacaError::InvalidValue);

    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE(mount.start_count(1) == ra_starts_before);
    REQUIRE(mount.start_count(2) == dec_starts_before);
    expect_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - superseded dispatch neither strands nor clobbers the other axis", "[skywatcher][async]") {
    // PR #216 rounds 4+6: when RA's stop-wait is superseded mid-dispatch,
    // the dispatch must abort without emitting stale stops — a MoveAxis that
    // legitimately claimed Dec during the wait keeps its motion (round 6),
    // and the abandoned dispatch leaves no inconsistent Slewing/tracking
    // bookkeeping behind (round 4).
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 3.0 + 24.0, 24.0), 25.0);
    // While the dispatch stop-waits the ramping RA axis, a concurrent client
    // starts a Dec MoveAxis — bumping the generation and claiming the axes.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->move_axis(1, 1.0);
    int dec_stops_after_claim = mount.stop_count(2);

    // Dec's fresh motion must SURVIVE the aborted dispatch: no stale stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(mount.stop_count(2) == dec_stops_after_claim);
    REQUIRE(mount.axis_running(2));
    REQUIRE(driver->get_slewing());  // the manual Dec motion reports Slewing

    // And the normal MoveAxis stop path still cleans up consistently.
    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 10000));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    driver->set_connected(false);
}

// QUARANTINED (issue #586): tagged [!mayfail] so it still runs and reports
// but cannot fail the gate. It exposes a REAL driver defect, not a flaky
// test -- issue #535: an in-flight SetTracking(false) is invisible to the
// MoveAxis(0) restore task, so the client's call throws and the mount is
// left tracking. Measured at 02346b6f on arm64 Debian 13: 10 failures in
// 30 standalone runs, 5 in 30 under load -- but 0 in three full
// `ctest -j 4` suite runs, so a filtered or sharded invocation is what
// hits it. Those figures are for the DEFAULT build type (9/30 on a
// re-measure); a CMAKE_BUILD_TYPE=Debug build saw 0/30, so reproduce at
// the default type before concluding anything about #535.
// REMOVE THIS TAG when #535 is fixed.
TEST_CASE("SkyWatcher async - MoveAxis stop restore yields to a newer tracking command",
          "[skywatcher][async][!mayfail]") {
    // PR #216 round-5 finding: the MoveAxis(0) background restore-tracking
    // task must not re-start tracking that a concurrent SetTracking(false)
    // stopped while the task was polling the deceleration.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    driver->move_axis(0, 2.0);
    REQUIRE(driver->get_slewing());
    driver->move_axis(0, 0.0);  // async stop; restore task polls the ramp
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->set_tracking(false);  // newer motion command supersedes the restore

    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE_FALSE(driver->get_tracking());
    REQUIRE(wait_until([&] { return !mount.axis_running(1); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - DeclinationRate drives Dec with the east-branch sign", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    // At the power-on position the Dec axis angle is 0 (east branch, a2 >= 0),
    // where dec = 90 - a2: +DeclinationRate must move the axis NEGATIVE.
    driver->set_declination_rate(10.0);  // arcsec/s, well above the ~0.26 floor
    REQUIRE(driver->get_declination_rate() == 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    double start = mount.physical_degrees(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    double moved_arcsec = (mount.physical_degrees(2) - start) * 3600.0;
    REQUIRE(moved_arcsec < -10.0);
    REQUIRE(moved_arcsec > -40.0);

    // Zeroing the rate stops the offset motion.
    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - RightAscensionRate offset is subtracted from the drive", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // +10 s/sidereal-s = +150 arcsec/s of RA drift; sidereal is ~15 arcsec/s,
    // so the RA axis must REVERSE (RA = LST - HA -> offset subtracts).
    driver->set_right_ascension_rate(10.0);
    REQUIRE(driver->get_right_ascension_rate() == 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    double start = mount.physical_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(mount.physical_degrees(1) < start);

    driver->set_right_ascension_rate(0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double resume = mount.physical_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE(mount.physical_degrees(1) > resume);  // back to plain sidereal
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - rate offsets require Sidereal and zero on drive-rate change", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    driver->set_tracking_rate(1);  // Lunar
    REQUIRE_THROWS_AS(driver->set_declination_rate(1.0), alpacacore::AlpacaException);
    REQUIRE_THROWS_AS(driver->set_right_ascension_rate(1.0), alpacacore::AlpacaException);

    driver->set_tracking_rate(0);  // Sidereal
    driver->set_declination_rate(5.0);
    driver->set_right_ascension_rate(2.0);
    driver->set_tracking_rate(1);  // ASCOM: drive-rate change zeroes offsets
    REQUIRE(driver->get_declination_rate() == 0.0);
    REQUIRE(driver->get_right_ascension_rate() == 0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - Dec pulse guide restores an active DeclinationRate offset", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east branch, well away from the pole

    driver->set_declination_rate(10.0);  // continuous (above-floor) offset
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));

    driver->pulse_guide(0, 600);  // North pulse pre-empts the offset motion
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));

    // The offset motion must resume by itself after the pulse ends.
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    double start = mount.physical_degrees(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE((mount.physical_degrees(2) - start) * 3600.0 < -7.0);  // still ~-10 as/s
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - MoveAxis Dec stop restores an active DeclinationRate offset", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east branch, well away from the pole

    driver->set_declination_rate(10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));

    driver->move_axis(1, 1.0);  // manual Dec nudge
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    driver->move_axis(1, 0.0);  // stop task must re-apply the offset
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 10000));

    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 5000));
    double start = mount.physical_degrees(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    REQUIRE((mount.physical_degrees(2) - start) * 3600.0 < -7.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - sub-floor DeclinationRate duty-cycles the axis", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    int starts = mount.start_count(2);
    int stops = mount.stop_count(2);
    driver->set_declination_rate(0.1);  // below the ~0.26 arcsec/s slow-mode floor
    // ~1.0s bursts on a 3s period: expect at least two on/off cycles in 7.5s.
    REQUIRE(wait_until([&] { return mount.start_count(2) >= starts + 2 && mount.stop_count(2) >= stops + 2; }, 7500));

    // Tracking off stops the bursts (the worker exits instead of idling).
    driver->set_tracking(false);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    int idle_starts = mount.start_count(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    REQUIRE(mount.start_count(2) == idle_starts);

    // Tracking back on resumes duty-cycling from the stored DeclinationRate.
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.start_count(2) > idle_starts; }, 7500));

    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - duty burst end does not truncate a concurrent Dec pulse", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    // 0.2 arcsec/s is sub-floor: ~2.2 s bursts on a 3 s period, so a pulse
    // dispatched inside a burst overlaps the burst's own end-of-burst stop.
    driver->set_declination_rate(0.2);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 7500));

    double before = mount.physical_degrees(2);
    driver->pulse_guide(0, 1500);  // North, 1.5 s at the 0.5x default rate
    REQUIRE(driver->get_is_pulse_guiding());
    // Mid-pulse (when the burst's off-timer fires) the axis must still run.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(mount.axis_running(2));
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    // Full-length pulse displacement (~11 arcsec), not a truncated one.
    double moved_arcsec = (mount.physical_degrees(2) - before) * 3600.0;
    REQUIRE(moved_arcsec < -6.0);
    REQUIRE(moved_arcsec > -20.0);

    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - idempotent SetTracking/rate rewrites do not churn the offset", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    driver->set_declination_rate(10.0);  // continuous offset motion
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    int stops = mount.stop_count(2);
    int starts = mount.start_count(2);

    // Keep-alive reassertion and same-value rewrites: common ASCOM client
    // behavior — must leave the running Dec offset motion untouched.
    for (int i = 0; i < 3; ++i) {
        driver->set_tracking(true);
        driver->set_declination_rate(10.0);
        driver->set_right_ascension_rate(0.0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(mount.stop_count(2) == stops);
    REQUIRE(mount.start_count(2) == starts);
    REQUIRE(mount.axis_running(2));

    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - rate setters during a goto defer instead of hijacking it", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    double target_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    driver->slew_to_coordinates_async(target_ra, 40.0);
    REQUIRE(driver->get_slewing());

    // Mid-flight rate writes must not replace the goto motion with
    // tracking-rate motion (which would read as "not slewing" and leave the
    // mount silently off-target).
    driver->set_declination_rate(10.0);
    driver->set_right_ascension_rate(1.0);
    REQUIRE(driver->get_slewing());

    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    REQUIRE(std::abs(driver->get_declination() - 40.0) < 0.05);

    // The deferred Dec offset is live after the landing restore.
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 5000));
    REQUIRE(driver->get_declination_rate() == 10.0);
    REQUIRE(driver->get_right_ascension_rate() == 1.0);

    driver->set_declination_rate(0.0);
    driver->set_right_ascension_rate(0.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - ConformU chained measured-rate choreography (RA offset)", "[skywatcher][async]") {
    // Mirrors ConformU 4.5's RightAscensionRate test: probe writes (including
    // direction reversals), a slew, then a low-rate write with the achieved
    // rate measured from reported RA over wall time. The 2026-08-23 hardware
    // failure of this exact sequence was Pi clock slew, not the driver — this
    // pins the driver side of it.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    for (double r : {0.0, 0.0033, -0.0033, 2.667, -2.667, 0.0}) {
        driver->set_right_ascension_rate(r);
    }
    double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 2.0 + 24.0, 24.0), 40.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));

    driver->set_right_ascension_rate(0.0033);
    double ra0 = driver->get_right_ascension();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i) {  // ConformU-style polling during the window
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        (void)driver->get_slewing();
        (void)driver->get_declination();
    }
    double ra1 = driver->get_right_ascension();
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double rate = (ra1 - ra0) * 3600.0 / dt;  // seconds of RA per SI second
    REQUIRE(rate > 0.0033 * 0.95);
    REQUIRE(rate < 0.0033 * 1.08);  // sidereal factor puts the exact value ~0.27% high

    driver->set_right_ascension_rate(0.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - RA offset canceling the drive stops the axis, not creeps", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // Offset of exactly +1.0 s-RA per sidereal second cancels the sidereal
    // drive: the axis must STOP (":I" would otherwise clamp at the ~0.26
    // arcsec/s floor and creep). Reported RA then advances at the LST rate.
    driver->set_right_ascension_rate(1.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(1); }, 5000));
    double phys0 = mount.physical_degrees(1);
    double ra0 = driver->get_right_ascension();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    REQUIRE(std::abs(mount.physical_degrees(1) - phys0) * 3600.0 < 0.5);  // no creep
    double drift = (driver->get_right_ascension() - ra0) * 3600.0 / 2.0;
    REQUIRE(drift > 0.9);  // ~+1.0027 s-RA/s reported
    REQUIRE(drift < 1.1);

    driver->set_right_ascension_rate(0.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 5000));  // sidereal resumes
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - sub-floor effective RA rate duty-cycles the RA axis", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // +0.99 s-RA/sidereal-s leaves ~0.15 arcsec/s of effective drive - below
    // the slow-mode floor: the axis must duty-cycle, never run continuously
    // at the clamped floor rate.
    int starts = mount.start_count(1);
    int stops = mount.stop_count(1);
    driver->set_right_ascension_rate(0.99);
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts + 2 && mount.stop_count(1) >= stops + 2; }, 9000));

    driver->set_right_ascension_rate(0.0);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 5000));  // back to continuous sidereal
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - Dec rate change does not orphan a live RA duty cycle", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);

    driver->set_right_ascension_rate(0.99);  // sub-floor effective RA: duty mode
    int starts = mount.start_count(1);
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts + 1; }, 7500));

    // A continuous (non-duty) Dec rate reaps the SHARED worker; it must be
    // restarted for the still-active RA duty cycle.
    driver->set_declination_rate(10.0);
    starts = mount.start_count(1);
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts + 2; }, 9000));

    driver->set_declination_rate(0.0);
    driver->set_right_ascension_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - rate offset entry keeps the reported RA continuous", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_right_ascension_rate(0.0);
    const double before = driver->get_right_ascension();

    // Hardware and model disagree by 18 arcsec (count quantization / start
    // latency, exaggerated): entering an offset must NOT re-anchor on
    // hardware and jump the reported RA — ConformU samples RA before the
    // rate write and 10 s after it, so a jump reads as a rate error.
    mount.jump_axis_degrees(1, 0.005);
    driver->set_right_ascension_rate(0.5);
    const double after = driver->get_right_ascension();
    REQUIRE(std::abs(after - before) < 3e-5);  // ~1.6 arcsec: model motion only

    driver->set_right_ascension_rate(0.0);
    driver->set_connected(false);
}

// ── EQM-35 Pro (Synta EQ board) ─────────────────────────────────────────
// The driver was written against the Wave 100i. These cases pin the behaviour
// that differs on a classic Synta board, using the geometry captured from real
// EQM-35 Pro hardware (see FakeMountProfile::eqm35_pro).

TEST_CASE("SkyWatcher EQM-35 - identity from the mount code byte", "[skywatcher][telescope][eqm35]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    // ":e" -> "=032732": firmware 3.39, mount code 0x32. The third byte is an
    // identity, NOT a patch level, so the version must read "3.39" and never
    // "3.39.50".
    CHECK(driver->get_name() == "Sky-Watcher EQM-35 Pro (EQMOD)");
    auto firmware = driver->get_device_firmware();
    REQUIRE(firmware.has_value());
    CHECK(*firmware == "3.39");

    // A clean disconnect must keep the last known-good identity: the web UI
    // and configureddevices listing read these while disconnected, and the
    // rig showed the direct connection reverting to a generic name after
    // every disconnect while the synscan driver kept its model (2026-09-10).
    driver->set_connected(false);
    CHECK(driver->get_name() == "Sky-Watcher EQM-35 Pro (EQMOD)");
    firmware = driver->get_device_firmware();
    REQUIRE(firmware.has_value());
    CHECK(*firmware == "3.39");
}

TEST_CASE("SkyWatcher EQM-35 - the connect log names the mount code in hex (#458)", "[skywatcher][telescope][eqm35]") {
    // measured_dec_axis_sense(), the instructions and #579 name boards as
    // 0x32, 0x45, ...; a bench reading taken from the log must not need a
    // decimal-to-hex conversion to find its row.
    std::atomic<int> hits{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink([&](alpacacore::logging::LogLevel, std::string_view, std::string_view message) {
        if (message.find("Motor board: EQM-35 Pro (mount code 0x32, 50)") != std::string_view::npos) {
            ++hits;
        }
    });
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    CHECK(hits.load() == 1);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher EQM-35 - FindHome uses the count-frame fallback", "[skywatcher][telescope][eqm35]") {
    // ":q" 0x000001 answers 0x7000 on this board: POLAR_LED |
    // COMMON_SLEW_START | HALF_CURRENT_TRACKING, with NO HOME_INDEXER (0x04).
    // The inquiry succeeds -- the bit is simply absent -- so the driver must
    // take the count-frame branch. This is the safety-relevant case: running
    // the AutoHome sensor hunt on a mount with no index sensors would drive
    // the axes looking for an edge that never arrives.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    // CanFindHome is unconditionally true by design
    // (.github/instructions/skywatcher.instructions.md): boards
    // without the sensor fall back to a goto of the power-on count frame.
    CHECK(driver->get_can_find_home() == true);

    // Move both axes away from the count home, then home them.
    mount.jump_axis_degrees(1, 5.0);
    mount.jump_axis_degrees(2, -4.0);

    driver->find_home();
    REQUIRE(wait_until([&] { return !driver->get_slewing() && driver->get_at_home(); }, 20000));

    // Landed on the count frame origin, not wherever a sensor hunt drifted to.
    CHECK(std::fabs(mount.axis_degrees(1)) < 0.2);
    CHECK(std::fabs(mount.axis_degrees(2)) < 0.2);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher Wave - home indexer still enables FindHome", "[skywatcher][telescope][eqm35]") {
    // Guard against the EQM-35 work regressing the Wave: same code path, the
    // 0x100C feature word, and FindHome must stay available.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    CHECK(driver->get_name() == "Sky-Watcher Wave 100i (EQMOD)");
    CHECK(driver->get_can_find_home() == true);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher EQM-35 - tracking uses the board's own sidereal period", "[skywatcher][telescope][eqm35]") {
    // The EQM-35's motor board reports its sidereal step period via ":D" as
    // 149592. The driver derives it independently as
    //   T1 = timer_freq * 360 / rate / CPR
    //      = 16e6 * 360 / 9216000 / (360.98564736629/86400 deg/s)
    // Agreement to ~1e-5 is what makes the Wave-derived rate math correct on
    // this mount unchanged, so assert the driver actually tracks at that rate.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    const double before = mount.physical_degrees(1);
    driver->set_tracking(true);
    CHECK(driver->get_tracking());

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const double after = mount.physical_degrees(1);

    // Sidereal is ~0.004178 deg/s; over 0.6 s that is ~2.5e-3 deg. Assert the
    // axis moved in the tracking direction at roughly the sidereal rate rather
    // than pinning an exact figure (the loopback clock is not real-time).
    const double moved = std::fabs(after - before);
    const double expected = FakeSkyWatcherMount::kSiderealDegPerSec * 0.6;
    CHECK(moved > expected * 0.3);
    CHECK(moved < expected * 3.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

// ── Southern hemisphere tracking direction ──────────────────────────────────

TEST_CASE("SkyWatcher southern hemisphere - tracking turns RA the right way",
          "[skywatcher][telescope][eqm35][hemisphere]") {
    // History: #250 removed the southern RA reversal from
    // start_speed_motion_locked() because the driver's REPORTED RA advanced
    // at 2x sidereal with it in place. That report came from a pointing
    // model that read the RA axis angle as the hour angle in both
    // hemispheres; #432 showed the model was wrong (the counterweight-down
    // home puts the dec-axis sweep on the HA = +/-6 h circle, and south of
    // the equator the mount faces the other pole, so HA = -(a1/15) +/- 6).
    // With the corrected model, holding a star below the equator needs the
    // counts to go DOWN -- indi-eqmod's `RAInverted = (Hemisphere == SOUTH)`.
    //
    // The RATE was correct the whole time (0.99995x sidereal on hardware);
    // only the direction is at stake, so this asserts the sign. The
    // hardware-anchored version of this assertion is in
    // test_skywatcher_pointing.cpp, which checks the same tracking sense
    // against the measured rows rather than against the driver's report.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0000, 150.0000, 80.0);
    driver->set_connected(true);

    const double before = mount.axis_degrees(1);
    driver->set_tracking(true);
    REQUIRE(driver->get_tracking());
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double after = mount.axis_degrees(1);

    // South of the equator the sky hour angle increases as the axis angle
    // DEcreases, so tracking must drive the counts down.
    INFO("axis1 moved from " << before << " to " << after << " deg");
    CHECK(after < before);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher northern hemisphere - tracking direction unchanged", "[skywatcher][telescope][hemisphere]") {
    // North of the equator increasing counts move the OTA west (EQMOD's
    // convention and what the Wave 100i was validated on), so tracking must
    // drive the counts UP; the southern sibling above asserts the mirror.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0);
    driver->set_connected(true);

    const double before = mount.axis_degrees(1);
    driver->set_tracking(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double after = mount.axis_degrees(1);

    INFO("axis1 moved from " << before << " to " << after << " deg");
    CHECK(after > before);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a SiteLatitude write across the equator re-applies the RA drive",
          "[skywatcher][telescope][hemisphere]") {
    // The drive direction comes from the latitude (effective_ra_rate_locked()),
    // so a client pushing its own site after connect -- the ordinary way in,
    // and the usual way a sign typo in the web-UI site config gets corrected --
    // can leave the axis running the way the OLD hemisphere wanted. Nothing
    // else re-applies until Tracking, TrackingRate, RightAscensionRate or a
    // slew happens to, so the axis holds the wrong direction at 1x and the star
    // trails at 2x: the #250 signature, from the other end.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double north_before = mount.axis_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double north_after = mount.axis_degrees(1);
    INFO("north: axis1 " << north_before << " -> " << north_after << " deg");
    REQUIRE(north_after > north_before);

    driver->set_site_latitude(-35.0);

    const double south_before = mount.axis_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double south_after = mount.axis_degrees(1);
    INFO("after crossing the equator: axis1 " << south_before << " -> " << south_after << " deg");
    CHECK(south_after < south_before);

    // A write that stays in the same hemisphere must not disturb the drive.
    driver->set_site_latitude(-37.2);
    const double same_before = mount.axis_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double same_after = mount.axis_degrees(1);
    INFO("same hemisphere: axis1 " << same_before << " -> " << same_after << " deg");
    CHECK(same_after < same_before);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher southern hemisphere - a positive RightAscensionRate still slows the axis",
          "[skywatcher][telescope][eqm35][hemisphere]") {
    // effective_ra_rate_locked() subtracts the offset and then applies the
    // hemisphere sign, so a positive RightAscensionRate has to make the sky
    // hour angle advance more slowly in both hemispheres. South of the equator
    // the axis rate is negative, so "slower" is a SMALLER magnitude, not a
    // smaller signed value. Only the plain tracking case covered this sign.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0000, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double plain_start = mount.axis_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double plain_travel = std::abs(mount.axis_degrees(1) - plain_start);

    driver->set_right_ascension_rate(driver->get_right_ascension_rate() + 0.5);

    const double offset_start = mount.axis_degrees(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const double offset_end = mount.axis_degrees(1);
    const double offset_travel = std::abs(offset_end - offset_start);

    INFO("south: plain travel " << plain_travel << " deg, with +0.5 s/s offset " << offset_travel << " deg");
    CHECK(offset_end < offset_start);     // still tracking the right way
    CHECK(offset_travel < plain_travel);  // and more slowly

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a SiteLatitude write during an RA pulse restores the NEW hemisphere's direction",
          "[skywatcher][telescope][hemisphere]") {
    // Review finding: set_site_latitude() skips a busy RA axis on the grounds
    // that the restore paths recompute -- but the pulse path captured
    // ra_restore_rate_deg_per_sec at DISPATCH and wrote it back verbatim at
    // pulse end. Autoguiding keeps pulse_axis_active_[i] true for most of
    // every guide cycle (PHD2: duration + 1 s), so a site correction made
    // mid-session lands here rather than in the setter's re-apply, and the
    // pulse restored the pre-write direction. RA then ran backwards until
    // something else re-applied the drive.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    const auto ra_drift = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return mount.physical_degrees(1) - p0;
    };
    const double north_drift = ra_drift();
    REQUIRE(std::abs(north_drift) > 0.0);

    // An East/West pulse keeps the RA axis busy; the site is corrected while
    // it is in flight, so the setter takes its busy-axis skip.
    driver->pulse_guide(2, 1200);  // East, 1.2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->set_site_latitude(-39.7392);

    // Once the pulse has restored tracking, the axis must be running the way
    // the NEW hemisphere wants. With the dispatch-time capture it keeps the
    // old direction and this comparison fails.
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 6000));
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const double south_drift = ra_drift();
    REQUIRE(std::abs(south_drift) > 0.0);
    CHECK((north_drift > 0.0) != (south_drift > 0.0));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a SiteLatitude write during a DEC pulse still re-applies the RA drive",
          "[skywatcher][telescope][hemisphere]") {
    // Round-2 review finding, the other half of the sibling above: the busy
    // skip was decided from axes_busy_locked(), which is true for ANY pulse,
    // but only an RA pulse's restore re-derives the RA drive. A North/South
    // pulse takes the stop_axis() else-branch -- it stops the DEC axis and
    // re-applies the Dec offset, and never touches RA. So the setter skipped,
    // the pulse skipped, and RA kept counting the old hemisphere's way
    // indefinitely: the 2x-trailing #250 signature again. Autoguiding makes
    // this the COMMON case -- roughly half of PHD2's corrections are Dec.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    const auto ra_drift = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return mount.physical_degrees(1) - p0;
    };
    const double north_drift = ra_drift();
    REQUIRE(std::abs(north_drift) > 0.0);

    // A North pulse keeps the DEC axis busy; the RA axis is untouched and
    // still tracking, so the setter must re-apply it rather than skip.
    driver->pulse_guide(0, 1200);  // North, 1.2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->set_site_latitude(-39.7392);

    // RA reverses immediately -- it is not the pulse's axis, so there is
    // nothing to wait for. Checked again after the pulse ends to prove the
    // Dec restore does not undo it.
    const double mid_drift = ra_drift();
    REQUIRE(std::abs(mid_drift) > 0.0);
    INFO("north RA drift " << north_drift << " deg, during the Dec pulse " << mid_drift << " deg");
    CHECK((north_drift > 0.0) != (mid_drift > 0.0));

    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 6000));
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const double south_drift = ra_drift();
    REQUIRE(std::abs(south_drift) > 0.0);
    INFO("after the Dec pulse: RA drift " << south_drift << " deg");
    CHECK((north_drift > 0.0) != (south_drift > 0.0));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a RightAscensionRate write during a long East pulse survives the post-stop verify",
          "[skywatcher][telescope][hemisphere]") {
    // Round-4 review note: stop_axis() re-derives the restore rate under the
    // lock -- that is what makes set_right_ascension_rate()'s RA-axis skip
    // safe, since the setter defers to "the busy operation's restore path".
    // But the post-stop verify that follows was still handed the DISPATCH-time
    // capture. With the two disagreeing, live_rate_change_took() measures the
    // correctly restored axis, finds it nearer the pulse rate than the stale
    // expectation, calls it "did not take" and resends ":I" at the pre-write
    // period. The client's offset is silently dropped and
    // cmd_axis_rate_deg_s_[0] stops describing what the axis is running.
    //
    // Needs a pulse at or past kMinPulseForRateVerifyMs (1500 ms) for that
    // verify to run at all, and an offset big enough to move the
    // classification: East, 2 s, +0.3 s/s (the threshold works out near
    // 0.25 s/s, and only on East).
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // past the ramp

    const auto ra_travel = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return std::abs(mount.physical_degrees(1) - p0);
    };
    const double plain_travel = ra_travel();
    REQUIRE(plain_travel > 0.0);

    driver->pulse_guide(2, 2000);  // East, 2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(driver->get_is_pulse_guiding());
    driver->set_right_ascension_rate(0.3);  // deferred: the RA axis is the pulse's
    REQUIRE(driver->get_right_ascension_rate() == 0.3);

    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // past the verify window

    // The restore applied sidereal * (1 - 0.3). Resent at the stale rate the
    // axis runs at plain sidereal and travels the full distance.
    const double offset_travel = ra_travel();
    INFO("plain travel " << plain_travel << " deg, with +0.3 s/s written mid-pulse " << offset_travel << " deg");
    CHECK(offset_travel < 0.85 * plain_travel);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a DeclinationRate write during an RA pulse is applied, not stranded",
          "[skywatcher][telescope][hemisphere]") {
    // Round-3 review finding, and the third instance of the same guard: the
    // whole-mount predicate reaches apply_dec_rate_offset_locked() as
    // defer_motion, so an East/West pulse -- which owns only the RA axis --
    // made it true and the continuous branch returned without starting any
    // Dec motion. The pulse's stop_axis() restore only rewrites the RA step
    // period and never calls apply_dec_rate_offset_locked(), so the offset
    // was stranded until the next DeclinationRate write, tracking toggle or
    // slew. Comet or satellite tracking while autoguiding is the way in.
    //
    // Only the CONTINUOUS branch: a sub-floor rate is recovered by the duty
    // worker's own start gate once the axes are free, so 10 arcsec/s (well
    // above the ~0.26 arcsec/s floor) is the rate that shows it.
    //
    // No hemisphere is involved, so this is northern like the RA sibling.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    REQUIRE_FALSE(mount.axis_running(2));

    // An East pulse owns the RA axis only, and is long enough that the Dec
    // motion below cannot be the pulse's own restore path.
    driver->pulse_guide(2, 3000);  // East, 3 s
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(driver->get_is_pulse_guiding());

    driver->set_declination_rate(10.0);  // arcsec/s, continuous (above the floor)
    REQUIRE(driver->get_declination_rate() == 10.0);

    // The Dec axis must start while the RA pulse is still in flight.
    CHECK(wait_until([&] { return mount.axis_running(2); }, 1500));
    CHECK(driver->get_is_pulse_guiding());

    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 8000));
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a RightAscensionRate write during a DEC pulse is applied, not stranded",
          "[skywatcher][telescope][hemisphere]") {
    // Round-2 review note: set_right_ascension_rate() asked the whole-mount
    // axes_busy_locked() for the same "the owner's restore will re-apply it"
    // skip that set_site_latitude() had to learn is a per-axis question. A
    // North/South pulse makes that predicate true while owning only the DEC
    // axis, and its restore path never touches RA, so the new rate sat in the
    // field with nothing scheduled to drive it -- the client's
    // RightAscensionRate silently did nothing until the next re-apply.
    //
    // Not hemisphere-specific (no sign is involved), which is why it needs its
    // own case rather than riding on the latitude ones above.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    const auto ra_travel = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return std::abs(mount.physical_degrees(1) - p0);
    };
    const double plain_travel = ra_travel();
    REQUIRE(plain_travel > 0.0);

    // A North pulse owns the Dec axis only; RA is still tracking.
    driver->pulse_guide(0, 1200);  // North, 1.2 s
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->set_right_ascension_rate(driver->get_right_ascension_rate() + 0.5);

    // +0.5 s/s slows the drive: the axis must already be travelling less far
    // per unit time, without waiting for the Dec pulse to end.
    const double offset_travel = ra_travel();
    INFO("plain travel " << plain_travel << " deg, with +0.5 s/s during a Dec pulse " << offset_travel << " deg");
    CHECK(offset_travel < plain_travel);

    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 6000));
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a SiteLatitude write during a DEC MoveAxis still re-applies the RA drive",
          "[skywatcher][telescope][hemisphere]") {
    // The MoveAxis half of the same finding: manual_axis_slewing_[1] made
    // axes_busy_locked() true, and the Dec stop task's restore only calls
    // apply_dec_rate_offset_locked() for channel == kAxisDec. RA was left on
    // the old hemisphere's direction with nothing scheduled to re-derive it.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    const auto ra_drift = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return mount.physical_degrees(1) - p0;
    };
    const double north_drift = ra_drift();
    REQUIRE(std::abs(north_drift) > 0.0);

    driver->move_axis(1, 0.5);  // Dec axis nudge, degrees/sec
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    driver->set_site_latitude(-39.7392);

    const double mid_drift = ra_drift();
    REQUIRE(std::abs(mid_drift) > 0.0);
    INFO("north RA drift " << north_drift << " deg, during the Dec MoveAxis " << mid_drift << " deg");
    CHECK((north_drift > 0.0) != (mid_drift > 0.0));

    driver->move_axis(1, 0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const double south_drift = ra_drift();
    REQUIRE(std::abs(south_drift) > 0.0);
    INFO("after the Dec MoveAxis stop: RA drift " << south_drift << " deg");
    CHECK((north_drift > 0.0) != (south_drift > 0.0));

    driver->set_tracking(false);
    driver->set_connected(false);
}

// ── Southern hemisphere Dec-rate sign (DeclinationRate / PulseGuide) ────────
//
// Found by static review, not on hardware: apply_dec_rate_offset_locked() and
// pulse_guide()'s North/South branch both flip axis direction with the plain
// rule "a2 >= 0 -> negate", derived from the NORTHERN pointing formula
// dec = 90 - a2 (d(dec)/d(a2) = -1 there). compute_ra_dec_locked() negates
// the whole dec value below the equator (dec_sky = -(90 - a2) = a2 - 90 on
// the same branch), which flips the SIGN of that derivative
// (d(dec_sky)/d(a2) = +1 south of the equator on the a2 >= 0 branch). Neither
// call site consulted hemisphere_south_locked(), so both carry the exact
// class of bug already found and fixed for RA tracking in
// start_speed_motion_locked() (see 48afe0d) -- just for Dec, and for
// DeclinationRate/PulseGuide instead of plain tracking.
//
// The hardware-validated MoveAxis data point
// (.github/instructions/skywatcher.instructions.md, EQM-35 Pro at
// latitude -37.2: pressing N increased reported Dec, i.e. a POSITIVE a1/a2
// axis rate on the a2 >= 0 branch increases sky Dec below the equator) is
// the independent check that the south-of-equator direction asserted here
// is the physically correct one, not just internally consistent.

TEST_CASE("SkyWatcher southern hemisphere - DeclinationRate drives Dec the right way",
          "[skywatcher][telescope][eqm35][hemisphere]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0000, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    // Power-on position: Dec axis angle 0 (east branch, a2 >= 0). South of
    // the equator that branch has dec_sky = a2 - 90, so +DeclinationRate
    // (increasing sky Dec) requires the axis to move POSITIVE -- the mirror
    // image of the northern-hemisphere assertion in the sibling test above.
    driver->set_declination_rate(10.0);  // arcsec/s, well above the ~0.26 floor
    REQUIRE(driver->get_declination_rate() == 10.0);
    REQUIRE(wait_until([&] { return mount.axis_running(2); }, 3000));
    double start = mount.physical_degrees(2);
    const double reported_dec_start = driver->get_declination();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    double moved_arcsec = (mount.physical_degrees(2) - start) * 3600.0;
    INFO("axis2 moved " << moved_arcsec << " arcsec");
    CHECK(moved_arcsec > 10.0);
    CHECK(moved_arcsec < 40.0);
    // The ASCOM contract, asserted against the driver's OWN pointing model:
    // a positive DeclinationRate must make the reported Declination rise.
    // Before the fix this read as Dec FALLING at 10 arcsec/s below the equator.
    const double reported_dec_end = driver->get_declination();
    INFO("reported Dec " << reported_dec_start << " -> " << reported_dec_end);
    CHECK(reported_dec_end > reported_dec_start);

    driver->set_declination_rate(0.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 5000));
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher southern hemisphere - pulse guide north moves Dec the right way",
          "[skywatcher][telescope][eqm35][hemisphere]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0000, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);
    mount.jump_axis_degrees(2, 45.0);  // east-pointing branch (a2 > 0)

    double dec_before = mount.axis_degrees(2);
    const double reported_dec_before = driver->get_declination();
    driver->pulse_guide(0, 1500);  // North, 1.5 s at the 0.5x default rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    // ~0.5x sidereal x 1.5 s ~ 11 arcsec; south of the equator the
    // east-branch sign rule makes +Dec (guide North) POSITIVE axis motion --
    // the mirror image of the northern-hemisphere test above.
    double moved_arcsec = (mount.axis_degrees(2) - dec_before) * 3600.0;
    INFO("axis2 moved " << moved_arcsec << " arcsec");
    CHECK(moved_arcsec > 6.0);
    CHECK(moved_arcsec < 20.0);
    REQUIRE(wait_until([&] { return !mount.axis_running(2); }, 3000));
    // The ASCOM contract, asserted against the driver's OWN pointing model: a
    // North pulse must leave the reported Declination higher than it started.
    // This is what an autoguider relies on -- before the fix a North
    // correction below the equator pushed the star further south.
    const double reported_dec_after = driver->get_declination();
    INFO("reported Dec " << reported_dec_before << " -> " << reported_dec_after);
    CHECK(reported_dec_after > reported_dec_before);
    driver->set_tracking(false);
    driver->set_connected(false);
}

// ── Pier side across the meridian (open-astro#261) ──────────────────────────
//
// These tests assert the ASCOM contract only: the reported side flips with
// hour angle and agrees with DestinationSideOfPier (the same shape OnStep's
// ConformU-validated fix above requires: "WE", not constant). Which
// MECHANICAL branch realises each side is the #261 question, and #432
// settled it: the goto picks the branch from the SKY hour angle, so
// HA >= 0 takes the branch with k * branch > 0, and get_side_of_pier() reads
// pierEast (0) back off it. Both profiles below run with k = +1 (EQM-35 Pro
// south, Wave 100i unmeasured), so that is the a2 >= 0 branch in both
// hemispheres, which is why the labels match the northern case. A board with
// a measured sense mirrors the branch where s * eps = -1 (#458); the label
// does not change.

TEST_CASE("SkyWatcher southern hemisphere - SideOfPier flips with hour angle and agrees with destination",
          "[skywatcher][telescope][eqm35][hemisphere]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0000, 150.0000, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    // West of the meridian (HA > 0) -> pierEast (0) on the a2 >= 0 branch.
    double west_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    // East of the meridian (HA < 0) -> pierWest (1) on the a2 < 0 branch.
    double east_ra = std::fmod(lst + 2.0, 24.0);
    const double dec = -40.0;

    REQUIRE(driver->get_destination_side_of_pier(west_ra, dec) == 0);
    REQUIRE(driver->get_destination_side_of_pier(east_ra, dec) == 1);

    driver->slew_to_coordinates_async(west_ra, dec);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(driver->get_side_of_pier() == 0);

    // A branch crossing swings the RA axis (a1) from +60 to -60 deg and the
    // Dec axis from +50 to -50 -- that IS a real meridian flip, not a test
    // artifact: at kMaxMoveAxisRateDegPerSec (~3.3 deg/s) the 120 deg RA leg
    // is a ~36 s goto plus ramp (measured 57 s end to end in the loopback),
    // not the ~15 s one-branch slew above. (This comment used to say "close
    // to 180 deg"; it was 120 under the old model too, so the figure was
    // always wrong -- corrected here because #432 rewrites the section
    // header just above.)
    driver->slew_to_coordinates_async(east_ra, dec);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 90000));
    // The branch must actually flip on the second goto, not just relabel the
    // same axis position.
    CHECK(driver->get_side_of_pier() == 1);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher northern hemisphere - SideOfPier flips with hour angle and agrees with destination",
          "[skywatcher][telescope][hemisphere]") {
    // Mirrors the southern-hemisphere test above with an unchanged (Wave)
    // profile: the side label follows the sky HA in both hemispheres, and both
    // profiles here run with k = +1, so the branch is the same too.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 39.7392, -104.9903, 1609.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    double lst = driver->get_sidereal_time();
    double west_ra = std::fmod(lst - 2.0 + 24.0, 24.0);
    double east_ra = std::fmod(lst + 2.0, 24.0);
    const double dec = 40.0;

    REQUIRE(driver->get_destination_side_of_pier(west_ra, dec) == 0);
    REQUIRE(driver->get_destination_side_of_pier(east_ra, dec) == 1);

    driver->slew_to_coordinates_async(west_ra, dec);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 30000));
    CHECK(driver->get_side_of_pier() == 0);

    driver->slew_to_coordinates_async(east_ra, dec);  // 120 deg RA jump, see sibling test
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 90000));
    CHECK(driver->get_side_of_pier() == 1);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a step-period readback mismatch is logged, not resent and not thrown",
          "[skywatcher][async]") {
    // 6b4988b read every ":I" preset back with ":i" and resent, then threw,
    // on a mismatch. Reverted to the contract INDI's skywatcherAPI.cpp and
    // indi-eqmod use (PR #1 review): a transport failure throws, what the
    // board STORED never does -- the rounding tolerance was measured on one
    // board, and on the real EQM-35 a matching readback proved nothing anyway
    // (the board stores a live preset without applying it; a follow-up commit
    // handles that). The fake acks and drops one write: the rate change must
    // return normally, the dropped write must not be resent, and the axis
    // must not be stopped.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);
    const int stops_before = mount.stop_count(1);

    mount.drop_step_period_writes(1, 1);
    REQUIRE_NOTHROW(driver->set_right_ascension_rate(0.5));  // live ":I" on the tracking axis

    REQUIRE(mount.step_period(1) == sidereal_preset);  // logged, not resent
    REQUIRE(mount.stop_count(1) == stops_before);      // and the axis was left running
    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a live step-period change the board stores but never spins up is re-kicked",
          "[skywatcher][async]") {
    // Same hardware failure as above, but the ":i" readback DID match what
    // was written (6b4988b's fix saw nothing to resend) -- ConformU still
    // failed, and count-sampling on the mount showed the axis holding
    // exactly its old rate through the whole pulse. The wrapper now follows
    // every live in-place ":I" with a ":J" (matching INDI's skywatcherAPI.cpp
    // recipe), and the driver double-checks by sampling the position across
    // a short window and re-kicking if the axis didn't actually change speed.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // Baseline sidereal physical rate, measured the same way the assertion
    // below re-measures it.
    auto measure_rate = [&] {
        double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        double p1 = mount.physical_degrees(1);
        return (p1 - p0) / 0.3;
    };
    const double sidereal_rate = measure_rate();
    const int stops_before = mount.stop_count(1);
    const int starts_before = mount.start_count(1);

    // Stall exactly one live write: the fake mount stores it (":i" agrees,
    // matching the real board's behavior) but the fake's own ":J" re-latch
    // (added for this fix) is what actually moves the axis at the new rate
    // -- proving the driver's ":J" kick reached the board.
    mount.stall_live_rate_writes(1, 1);
    driver->set_right_ascension_rate(0.5);  // continuous, same direction: live ":I" on the tracking axis

    REQUIRE(mount.start_count(1) > starts_before);  // the ":J" kick reached the board
    REQUIRE(mount.stop_count(1) == stops_before);   // never a stop/restart, only a kick
    // The axis is ACTUALLY running at the new, non-sidereal rate now --
    // not silently stuck at the old one (a matching ":i" readback is not
    // enough, per the real hardware failure this models).
    const double new_rate = measure_rate();
    REQUIRE(std::abs(new_rate - sidereal_rate) > std::abs(sidereal_rate) * 0.1);

    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a ':J' failure after the pulse-rate ':I' restores the drive rate",
          "[skywatcher][async]") {
    // The live-rate pulse dispatch writes ":I" (pulse rate) then ":J". If the
    // ":J" throws, the ":I" has already gone out and the board may well have
    // applied it, so the axis is running at the guide rate with the pulse
    // aborted and nothing scheduled to bring it back (#249 review). The
    // dispatch failure path must restore the drive rate.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);
    const int stops_before = mount.stop_count(1);

    mount.reject_start_motion(1, 1);  // the ":J" after the pulse-rate ":I" is refused
    driver->pulse_guide(3, 5000);     // West: sidereal + guide rate, same direction -> live ":I"
    // The dispatch fails, the pulse is abandoned...
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 3000));
    // ...and the axis is back on the drive rate, never stopped.
    REQUIRE(mount.step_period(1) == sidereal_preset);
    REQUIRE(mount.axis_running(1));
    REQUIRE(mount.stop_count(1) == stops_before);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the rate-applied check still catches a stall at a small guide rate",
          "[skywatcher][async]") {
    // Fork PR #6 review: the check used a fixed 25% tolerance on the expected
    // pulse rate, so at guide rates below ~0.33x sidereal (East) / ~0.2x
    // (West) an axis still stuck at sidereal read as "rate applied" and the
    // re-kick never fired -- and 0.1-0.3x is a common autoguider setting. The
    // check now classifies the observed rate by which commanded rate it is
    // nearer to. Model a stall that survives the dispatch's own ":I"+":J"
    // (the fake stores the preset, ignores one kick) at 0.1x sidereal and
    // assert the sampled check re-kicks within the pulse.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_guide_rate(
        {0.1 * FakeSkyWatcherMount::kSiderealDegPerSec, 0.1 * FakeSkyWatcherMount::kSiderealDegPerSec});
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);
    const int stops_before = mount.stop_count(1);

    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->pulse_guide(2, 3000);  // East, long enough for the ~450 ms sampled check
    REQUIRE(driver->get_is_pulse_guiding());

    // Dispatch sends one ":J"; only the re-kick sends a second one before the
    // end-of-pulse restore (which cannot arrive before the 3 s hold expires).
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts_before + 2; }, 1500));
    REQUIRE(mount.stop_count(1) == stops_before);  // a kick, never a stop/restart

    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    REQUIRE(mount.axis_running(1));  // tracking restored after the pulse
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a short RA guide pulse is not stretched by the rate-applied check",
          "[skywatcher][async]") {
    // The rate-applied check (verify_live_rate_or_rekick) samples the axis
    // for ~450 ms DURING the pulse. On real hardware (EQM-35 Pro,
    // 2026-09-07) counting that window twice overshot a 5 s ConformU pulse
    // by ~9% (RA change 2.74s vs 2.51s expected). A real autoguider sends
    // 50-500 ms pulses, where the check would BE the pulse and no deduction
    // could give the time back -- so short pulses must skip it entirely and
    // keep the requested ON time.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);

    constexpr int kPulseMs = 150;      // typical autoguider correction
    driver->pulse_guide(2, kPulseMs);  // East: live in-place rate change
    // Time the ON window itself: from the pulse step period landing on the
    // axis to the sidereal preset being restored. pulse_guide() is
    // asynchronous, so the change appears only once the task dispatches.
    REQUIRE(wait_until([&] { return mount.step_period(1) != sidereal_preset; }, 3000));
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(wait_until([&] { return mount.step_period(1) == sidereal_preset; }, 5000));
    double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Upper bound sits well below the ~450 ms sample window: if the check
    // ever runs on a pulse this short, the ON time jumps to ~450 ms and this
    // fails. Lower bound is loose (poll granularity only).
    REQUIRE(elapsed_ms >= 30.0);
    REQUIRE(elapsed_ms < 300.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a RightAscensionRate stall that survives the :J kick is caught in the background",
          "[skywatcher][async]") {
    // open-astro/AlpacaBridge#248: the RightAscensionRate / TrackingRate
    // setters (apply_ra_tracking_rate_locked) got the ":J" kick but not the
    // sampled rate-applied check -- they run under mutex_ inside a property
    // call and the check needs an unlocked ~450 ms window. A stall there has
    // no natural end point: RA would track at the wrong rate until the next
    // rate change. The setter now spawns a one-shot background check that
    // re-kicks the axis, without the property call itself waiting for it.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    auto measure_rate = [&] {
        double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        double p1 = mount.physical_degrees(1);
        return (p1 - p0) / 0.3;
    };
    const double sidereal_rate = measure_rate();
    const int starts_before = mount.start_count(1);
    const int stops_before = mount.stop_count(1);

    // A stall that survives the setter's own ":I"+":J": the preset is stored
    // (":i" agrees) and the kick is acknowledged but swallowed. Only the
    // sampled check can recover this.
    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    const auto t0 = std::chrono::steady_clock::now();
    driver->set_right_ascension_rate(0.5);  // continuous, same direction: live ":I" on the tracking axis
    const double setter_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // The property call must not absorb the ~450 ms sample window.
    REQUIRE(setter_ms < 200.0);

    // The setter's kick, then the background check's re-kick -- and never a
    // stop/restart, the axis keeps running throughout.
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts_before + 2; }, 1500));
    REQUIRE(mount.stop_count(1) == stops_before);
    const double new_rate = measure_rate();
    REQUIRE(std::abs(new_rate - sidereal_rate) > std::abs(sidereal_rate) * 0.1);

    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a pending RightAscensionRate check is reaped by Tracking off and by disconnect",
          "[skywatcher][async]") {
    // The background check is owned like the pulse task: whatever takes the
    // RA axis while its sample window is open reaps it, so its ":I"+":J"
    // resend can never land on an axis someone else just stopped (which
    // would silently restart tracking), and a disconnect joins it instead of
    // leaking a thread into the destructor.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);

    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->set_right_ascension_rate(0.5);
    driver->set_tracking(false);  // lands inside the check's ~450 ms sample window
    REQUIRE(wait_until([&] { return !mount.axis_running(1); }, 3000));
    // Give a leaked check its whole window and then some: nothing may
    // restart the axis, and no second ":J" may reach the board.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    REQUIRE_FALSE(mount.axis_running(1));
    REQUIRE(mount.start_count(1) == starts_before + 1);  // the setter's own kick only

    // Disconnect racing a fresh check: set_connected(false) must return
    // promptly (the check is cancelled, not waited out) and cleanly.
    driver->set_tracking(true);  // offset 0.5 still stored: restarts at the offset rate
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->set_right_ascension_rate(0.0);  // live change back to sidereal: spawns a check
    const auto t0 = std::chrono::steady_clock::now();
    driver->set_connected(false);
    const double disconnect_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(disconnect_ms < 2000.0);
    REQUIRE_FALSE(driver->get_connected());
}

TEST_CASE("SkyWatcher async - the rate-applied check stretches its window to resolve a Lunar TrackingRate stall",
          "[skywatcher][async]") {
    // Hardware 2026-09-10 (EQM-35 Pro): a TrackingRate=Lunar write produced a
    // spurious "did not take" + resend. Lunar is 3.5% off sidereal -- about
    // one count over the fixed 300 ms window, inside the two-read truncation
    // error, so the nearest-rate verdict was a coin flip. The window now
    // stretches until the two candidate rates are >= 4 counts apart (this
    // fake: ~2.4 s), so a REAL Lunar stall is still caught...
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);
    const int stops_before = mount.stop_count(1);
    const uint32_t sidereal_preset = mount.step_period(1);

    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->set_tracking_rate(1);                      // Lunar: live in-place ":I", 3.5% slower
    REQUIRE(mount.step_period(1) != sidereal_preset);  // stored...
    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts_before + 2; }, 4500));  // ...and re-kicked
    REQUIRE(mount.stop_count(1) == stops_before);

    driver->set_tracking_rate(0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a sub-resolution TrackingRate change is not spuriously re-kicked",
          "[skywatcher][async]") {
    // ...while Solar (0.27% off sidereal: 0.09 counts over 300 ms, ~30 s to
    // resolve) is below anything the check can see inside its 3 s cap, so it
    // must NOT sample-and-guess: exactly one ":J" (the setter's own kick),
    // never a resend, on a healthy board.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);

    driver->set_tracking_rate(2);                                  // Solar
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // well past settle + min window
    REQUIRE(mount.start_count(1) == starts_before + 1);

    driver->set_tracking_rate(0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE(
    "SkyWatcher async - a pulse whose rate delta is unresolvable within its own duration "
    "does not overshoot the commanded on-time",
    "[skywatcher][async]") {
    // Bot review round 1 on open-astro/AlpacaBridge#248: the pulse dispatch's
    // rate-applied check samples the axis WHILE it is already running at the
    // pulse rate, and the pulse's remaining hold is duration MINUS the time
    // the check took -- clamped at zero, never extended. Before this fix,
    // a low guide rate (small pulse-vs-tracking delta) at a duration right at
    // kMinPulseForRateVerifyMs could stretch the adaptive window toward its
    // 3 s ceiling, well past the 1.5 s commanded duration: the pulse would
    // physically hold the guide rate for however long the check took, over
    // 2x its commanded on-time. The dispatch call now caps its window at
    // (duration - settle), so an unresolvable delta is skipped immediately
    // (an INFO log, not a wait) instead of stretching past the pulse itself.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    // 0.02x sidereal: on this fake's counts-per-revolution, resolving this
    // delta to kMinResolvableDeltaCounts needs several seconds -- more than
    // (kMinPulseForRateVerifyMs - settle) leaves room for.
    driver->set_guide_rate(
        {0.02 * FakeSkyWatcherMount::kSiderealDegPerSec, 0.02 * FakeSkyWatcherMount::kSiderealDegPerSec});
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const uint32_t sidereal_preset = mount.step_period(1);

    constexpr int kPulseMs = 1500;  // exactly kMinPulseForRateVerifyMs: the check DOES run
    driver->pulse_guide(2, kPulseMs);
    REQUIRE(wait_until([&] { return mount.step_period(1) != sidereal_preset; }, 3000));
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(wait_until([&] { return mount.step_period(1) == sidereal_preset; }, 5000));
    double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Must land close to the commanded 1500 ms, nowhere near the ~3150 ms an
    // unbounded window would have produced.
    REQUIRE(elapsed_ms >= 1300.0);
    REQUIRE(elapsed_ms < 2000.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - re-asserting the same TrackingRate leaves a pending rate check running",
          "[skywatcher][async]") {
    // open-astro/AlpacaBridge#258 review: apply_ra_tracking_rate_locked()
    // reaped any pending check unconditionally, BEFORE its own "nothing
    // changed" early return. set_tracking_rate() has no idempotent-rewrite
    // guard (unlike set_right_ascension_rate), so a client re-asserting the
    // same TrackingRate mid-check cancelled it and spawned no replacement:
    // a stalled ":I" from the first write was then never caught -- the
    // exact unbounded-stall failure the background check exists for.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);
    const int stops_before = mount.stop_count(1);

    // Lunar stall that survives the setter's own ":I"+":J" (fake's lower
    // CPR needs ~2.4 s of window to resolve, so the check is still in
    // flight when the rewrite lands).
    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->set_tracking_rate(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // inside the sample window
    driver->set_tracking_rate(1);                                 // same value: must NOT drop the check
    REQUIRE(mount.start_count(1) == starts_before + 1);           // and must not write/kick again itself

    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts_before + 2; }, 4500));  // check re-kicked
    REQUIRE(mount.stop_count(1) == stops_before);

    driver->set_tracking_rate(0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a Dec pulse leaves a pending RA rate check running", "[skywatcher][async]") {
    // open-astro/AlpacaBridge#258 review: the pulse dispatch reaped a pending
    // RA rate-verify check unconditionally, so a North/South pulse (Dec axis
    // only) cancelled it with nothing to replace it. Dec corrections landing
    // inside the check's window are routine while autoguiding; a stalled
    // ":I" from a RightAscensionRate write would then never be caught.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    const int starts_before = mount.start_count(1);
    const int stops_before = mount.stop_count(1);

    mount.stall_live_rate_writes(1, 1);
    mount.ignore_start_relatches(1, 1);
    driver->set_right_ascension_rate(0.5);                        // spawns the check
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // inside its settle/window
    driver->pulse_guide(0, 200);                                  // North: Dec axis only
    REQUIRE(mount.start_count(1) == starts_before + 1);           // the pulse itself touched no RA

    REQUIRE(wait_until([&] { return mount.start_count(1) >= starts_before + 2; }, 1500));  // check re-kicked
    REQUIRE(mount.stop_count(1) == stops_before);
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 5000));

    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a client UTCDate write moves SiderealTime only on an undisciplined host (#287, #301)",
          "[skywatcher][async]") {
    // Before #287, get_utc_date() reported the client's offset while every LST
    // computation used the raw host clock, so a client time-sync fixed the
    // readback and not the pointing.
    //
    // Since open-astro#301 the two are deliberately split again, but only one
    // way round: the UTCDate readback ALWAYS honours the client's write, while
    // the pointing math honours it only on a host the kernel reports as
    // undisciplined. On an NTP-disciplined host the host clock is the better
    // one and the router has already refused to step it, so a client's error
    // must not reach the mount. open-astro#395: the discipline probe is a
    // seam, so both branches run on every build host; this case runs the body
    // once per branch.
    const bool host_disciplined = GENERATE(true, false);
    const ProbeGuard probe_guard(host_disciplined);
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE(driver->get_connected());

    const auto wrap24 = [](double h) {
        h = std::fmod(h, 24.0);
        return h < 0.0 ? h + 24.0 : h;
    };
    const double lst0 = driver->get_sidereal_time();
    const double ra0 = driver->get_right_ascension();
    const auto host_now = std::chrono::system_clock::now();
    driver->set_utc_date(host_now + std::chrono::hours(1));

    const auto reported = driver->get_utc_date();
    const auto readback_error = std::chrono::duration_cast<std::chrono::milliseconds>(
        reported - (std::chrono::system_clock::now() + std::chrono::hours(1)));
    CHECK(std::abs(readback_error.count()) < 500);

    // The axes have not moved (tracking off, counts fixed) so reported RA
    // follows LST one-for-one: RA = LST - HA.
    const double d_lst = wrap24(driver->get_sidereal_time() - lst0);
    const double d_ra = wrap24(driver->get_right_ascension() - ra0);
    if (host_disciplined) {
        // #301: the client's hour never reaches the pointing math. Both
        // deltas are the few milliseconds the test itself took.
        CHECK((d_lst < 0.002 || d_lst > 23.998));
        CHECK((d_ra < 0.002 || d_ra > 23.998));
    } else {
        // One UT hour is 1.0027379 sidereal hours.
        CHECK(d_lst > 1.0027379 - 0.002);
        CHECK(d_lst < 1.0027379 + 0.002);
        CHECK(d_ra > 1.0027379 - 0.002);
        CHECK(d_ra < 1.0027379 + 0.002);
    }

    // Setting the clock back to the host's time leaves LST where it started,
    // on either branch.
    driver->set_utc_date(std::chrono::system_clock::now());
    const double d_back = wrap24(driver->get_sidereal_time() - lst0);
    CHECK((d_back < 0.002 || d_back > 23.998));
    // ...and the readback follows the new write, again on either branch.
    const auto reported_back = driver->get_utc_date();
    const auto back_error =
        std::chrono::duration_cast<std::chrono::milliseconds>(reported_back - std::chrono::system_clock::now());
    CHECK(std::abs(back_error.count()) < 500);

    // open-astro#414: the offset is session state. Arm it again, then
    // disconnect and reconnect: the readback is back on the host clock until
    // the client writes UTCDate once more.
    driver->set_utc_date(std::chrono::system_clock::now() + std::chrono::hours(1));
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const auto after_reconnect = std::chrono::duration_cast<std::chrono::milliseconds>(
        driver->get_utc_date() - std::chrono::system_clock::now());
    CHECK(std::abs(after_reconnect.count()) < 500);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - the pointing clock ignores a client offset on an NTP-disciplined host (#301)",
          "[skywatcher][unit]") {
    // The ASCOM UTCDate readback is the client's property to set and always
    // honours the write; this rule is only about the clock the mount is aimed
    // by. Pure, so it is testable without an NTP daemon and without stepping
    // the test host's clock.
    using alpacacore::vendor::skywatcher::detail::pointing_uses_client_offset;

    // No surviving offset: nothing to apply, whatever the host is doing. That
    // covers both "the client never wrote one" and "the host clock was
    // stepped afterwards, so the delta describes a clock that no longer
    // exists" (#291 review) -- client_offset_survives_locked() collapses the
    // two before the rule is asked.
    CHECK_FALSE(pointing_uses_client_offset(false, false));
    CHECK_FALSE(pointing_uses_client_offset(false, true));

    // The off-grid case #289 exists for: no NTP, so the client's time is the
    // only correct time the host will ever see, and it must reach the mount.
    CHECK(pointing_uses_client_offset(true, false));

    // An NTP-disciplined host has the better clock, and the router already
    // refused to step it. A tablet 30 minutes out must not skew every goto by
    // 7.5 degrees of RA on a rig whose own time is good.
    CHECK_FALSE(pointing_uses_client_offset(true, true));
}

TEST_CASE("SkyWatcher async - the UTCDate readback honours the client on any host (#301)", "[skywatcher][async]") {
    // Whatever the pointing math does, a client that writes UTCDate and reads
    // it back must get its own value: ConformU checks exactly this, and it has
    // to hold on an NTP-disciplined build box as much as on an off-grid Pi.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE(driver->get_connected());

    for (const auto skew : {std::chrono::minutes(37), std::chrono::minutes(-37)}) {
        driver->set_utc_date(std::chrono::system_clock::now() + skew);
        const auto error = std::chrono::duration_cast<std::chrono::milliseconds>(
            driver->get_utc_date() - (std::chrono::system_clock::now() + skew));
        CHECK(std::abs(error.count()) < 500);
    }
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - connecting without configured site coordinates is refused (#274)",
          "[skywatcher][async]") {
    // The mount stores no site of its own, so an unconfigured device would run
    // on 0.0/0.0. hemisphere_south_locked() is site_latitude_ < 0.0, which
    // silently puts a southern rig on northern pointing math and undoes #250,
    // #253 and #261. 0.0/0.0 is a real place, so the driver tracks provenance
    // rather than testing for the magic value.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());

    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), std::nullopt, std::nullopt, std::nullopt);
    try {
        driver->set_connected(true);
        FAIL("Expected the connect to be refused");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("SkyWatcher async - one configured coordinate is not enough (#274)", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());

    // Latitude alone: the hemisphere is known but LST is not, so this is still
    // refused rather than half-accepted.
    // The error code is asserted, not just the type: a connect refused for an
    // unrelated reason (a fake-board handshake failure, say) also throws
    // AlpacaException, and these cases are about the site guard specifically.
    auto refused_for_site = [](alpacacore::TelescopeDriver& driver) {
        try {
            driver.set_connected(true);
            FAIL("Expected the connect to be refused");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        }
        CHECK_FALSE(driver.get_connected());
    };

    auto lat_only = sw::create_skywatcher_telescope(0, endpoint(mount), -33.87, std::nullopt, std::nullopt);
    refused_for_site(*lat_only);

    auto lon_only = sw::create_skywatcher_telescope(0, endpoint(mount), std::nullopt, 151.21, std::nullopt);
    refused_for_site(*lon_only);
}

TEST_CASE("SkyWatcher async - 0.0/0.0 configured explicitly is accepted (#274)", "[skywatcher][async]") {
    // Null island is a real place. The guard is about provenance, not about
    // the value, so a device deliberately configured there must connect.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());

    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 0.0, 0.0, 0.0);
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    CHECK(driver->get_site_latitude() == 0.0);
    CHECK(driver->get_site_longitude() == 0.0);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the ASCOM setters satisfy the site requirement (#274)", "[skywatcher][async]") {
    // A raw Alpaca client that writes SiteLatitude and SiteLongitude before
    // Connected has supplied the same information the config would have, so
    // the connect must succeed. Both setters work while disconnected.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());

    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), std::nullopt, std::nullopt, std::nullopt);
    driver->set_site_latitude(-33.87);
    // Still short one coordinate.
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);

    driver->set_site_longitude(151.21);
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher - a host clock step drops the client UTCDate offset (#291 review)", "[skywatcher][unit]") {
    // The offset is a delta against the host clock at write time. When the
    // host clock is corrected afterwards (Sync Time, NTP, `date`), applying
    // the stale delta on top of it would move every LST-derived value by the
    // old error, so client_offset_survives_locked() drops it -- for the
    // UTCDate readback as well as for pointing, since both time paths ask it
    // (#301). The rule is pure: the system clock and the steady clock must
    // have advanced by the same amount.
    using namespace std::chrono;
    using alpacacore::vendor::skywatcher::detail::host_clock_stepped;
    // Both clocks advanced together: no step.
    CHECK_FALSE(host_clock_stepped(seconds(90), seconds(90)));
    CHECK_FALSE(host_clock_stepped(milliseconds(90400), milliseconds(90000)));
    // Host clock jumped 20 minutes forward (Sync Time on a slow clock) or
    // 20 minutes back while the steady clock advanced 90 s: stepped.
    CHECK(host_clock_stepped(seconds(90) + minutes(20), seconds(90)));
    CHECK(host_clock_stepped(seconds(90) - minutes(20), seconds(90)));
    // Right at the tolerance edge: 1 s drift is not a step, 1.5 s is.
    CHECK_FALSE(host_clock_stepped(seconds(91), seconds(90)));
    CHECK(host_clock_stepped(milliseconds(91500), seconds(90)));
}

TEST_CASE("SkyWatcher async - target properties are independently set (#304, #391)", "[skywatcher][async]") {
    // Moved here from the unit file when the getters gained check_connected()
    // (#391): ASCOM treats the two target properties as independent, each
    // throwing ValueNotSet until that property itself has been written.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE(driver->get_connected());

    expect_alpaca_error([&] { driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    expect_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);

    // Writing RA must not unlock Dec.
    driver->set_target_right_ascension(7.25);
    CHECK(driver->get_target_right_ascension() == 7.25);
    expect_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::ValueNotSet);
    expect_alpaca_error([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::ValueNotSet);

    // Writing Dec unlocks the second property without disturbing the first.
    driver->set_target_declination(-12.5);
    CHECK(driver->get_target_right_ascension() == 7.25);
    CHECK(driver->get_target_declination() == -12.5);

    // Updating one leaves the other intact.
    driver->set_target_right_ascension(3.0);
    CHECK(driver->get_target_declination() == -12.5);
    driver->set_connected(false);

    // Disconnected, the getters say NotConnected first, whatever was set.
    expect_alpaca_error([&] { driver->get_target_right_ascension(); }, alpacacore::AlpacaError::NotConnected);
    expect_alpaca_error([&] { driver->get_target_declination(); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("SkyWatcher async - Dec written first leaves RA unset (#304)", "[skywatcher][async]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_target_declination(41.0);
    CHECK(driver->get_target_declination() == 41.0);
    expect_alpaca_error([&] { driver->get_target_right_ascension(); }, alpacacore::AlpacaError::ValueNotSet);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a slew refused at dispatch still publishes the target (#404)", "[skywatcher][async]") {
    // The three writers agree: the target is what the client asked for, set
    // before dispatch. The fake refuses the next ":J" start, so the
    // synchronous slew throws at dispatch; the target must still read back.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    mount.reject_start_motion(1, 1);  // kAxisRa; the goto starts RA first, so that throw is the dispatch failure
    CHECK_THROWS_AS(driver->slew_to_coordinates(5.5, -25.0), alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_slewing());
    CHECK(std::abs(driver->get_target_right_ascension() - 5.5) < 1e-9);
    CHECK(std::abs(driver->get_target_declination() + 25.0) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a sync that fails after its writes still publishes the target (#404)",
          "[skywatcher][async]") {
    // The sync half of #404: with tracking on, the sync stops RA, writes both
    // ":E" positions, then restarts tracking. The fake refuses that restart
    // (":J" answered "!2"), so sync_to_coordinates() throws after the point
    // the old code published the target. The pair must still read back.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 5000));
    mount.reject_start_motion(1, 1);
    CHECK_THROWS_AS(driver->sync_to_coordinates(5.5, -25.0), alpacacore::AlpacaException);
    CHECK(std::abs(driver->get_target_right_ascension() - 5.5) < 1e-9);
    CHECK(std::abs(driver->get_target_declination() + 25.0) < 1e-9);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the client-clock disagreement WARN fires once per connection (#400)",
          "[skywatcher][async]") {
    // Only meaningful on a disciplined host (the WARN is gated on it); on an
    // undisciplined runner the count stays 0 on both writes and the case
    // still passes, which is the honest outcome without a discipline seam.
    // The sink is restored by a guard, so a REQUIRE that throws out of the
    // case cannot leave the global sink pointing at this frame's counter.
    std::atomic<int> warns{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn &&
                message.find("Client UTCDate disagrees") != std::string::npos) {
                ++warns;
            }
        });
    {
        FakeSkyWatcherMount mount;
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        const auto far = std::chrono::system_clock::now() + std::chrono::minutes(30);
        driver->set_utc_date(far);
        driver->set_utc_date(far + std::chrono::seconds(1));
        driver->set_utc_date(far + std::chrono::seconds(2));
        const int first_session = warns.load();
        CHECK(first_session <= 1);
        // A reconnect re-arms it.
        driver->set_connected(false);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        driver->set_utc_date(far);
        CHECK(warns.load() == first_session * 2);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher async - discipline gained after the write stops the client offset steering pointing (#405)",
          "[skywatcher][async]") {
    // Host undisciplined at the write: the client's +1 h reaches LST. The
    // host then becomes disciplined without a step (NTP slewing a clock that
    // was already close), which the step detector cannot see. The pointing
    // path re-samples the probe at most once per interval and drops back to
    // the host clock; the UTCDate readback keeps honouring the client.
    // Process-wide probe state is installed and restored by the guard, so a
    // REQUIRE that throws out of the case cannot leave a lambda that
    // captures this frame in the global slot.
    std::atomic<bool> disciplined{false};
    const ProbeGuard probe_guard([&] { return disciplined.load(); }, std::chrono::milliseconds(50));
    {
        FakeSkyWatcherMount mount;
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        const auto wrap24 = [](double h) {
            h = std::fmod(h, 24.0);
            return h < 0.0 ? h + 24.0 : h;
        };
        const double lst0 = driver->get_sidereal_time();
        driver->set_utc_date(std::chrono::system_clock::now() + std::chrono::hours(1));
        const double d_before = wrap24(driver->get_sidereal_time() - lst0);
        CHECK(d_before > 1.0027379 - 0.002);

        disciplined = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        static_cast<void>(driver->get_sidereal_time());  // the read that re-samples
        const double d_after = wrap24(driver->get_sidereal_time() - lst0);
        CHECK((d_after < 0.002 || d_after > 23.998));
        // The readback is the client's property and still says +1 h.
        const auto readback = std::chrono::duration_cast<std::chrono::milliseconds>(
            driver->get_utc_date() - (std::chrono::system_clock::now() + std::chrono::hours(1)));
        CHECK(std::abs(readback.count()) < 500);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher async - syncing by coordinates sets both target flags (#304)", "[skywatcher][async]") {
    // The split half of #304 that the unit cases do not reach: the three
    // writers that set BOTH coordinates at once must keep doing so. A future
    // edit that dropped one assignment would leave the other target property
    // throwing ValueNotSet after a sync, and nothing else in the suite would
    // notice -- the unit cases only exercise the per-property setters.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    // Nothing is set on a fresh connect: reset_runtime_state_locked() clears
    // both, which is what makes ConformU's read-before-write check pass.
    CHECK_THROWS_AS(driver->get_target_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_target_declination(), alpacacore::AlpacaException);

    // Sync rather than slew: it sets the same pair through the same locked
    // path and returns without leaving a task running.
    driver->sync_to_coordinates(5.5, -25.0);
    CHECK(std::abs(driver->get_target_right_ascension() - 5.5) < 1e-9);
    CHECK(std::abs(driver->get_target_declination() + 25.0) < 1e-9);

    // And a reconnect clears both again, so the pair never survives a session.
    // get_connected() is asserted between the two calls on purpose: without
    // it the case is self-satisfying, since a reconnect that silently failed
    // would leave both getters throwing and everything below would pass.
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_THROWS_AS(driver->get_target_right_ascension(), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->get_target_declination(), alpacacore::AlpacaException);
    driver->set_connected(false);
}

// ---------------------------------------------------------------------------
// PR #448: the post-slew tracking-rate check, the landing-settle wait, and the
// Slewing/restore ordering. Before these, deleting either
// verify_post_slew_tracking_rate_locked() or wait_axis_stationary_locked()
// left the suite green.
// ---------------------------------------------------------------------------

TEST_CASE("SkyWatcher async - a goto whose tracking restarts at the wrong rate is stopped and restarted",
          "[skywatcher][async]") {
    // The #432 failure: the goto lands, tracking is re-applied, the board
    // acknowledges it and ":i" reads back exactly what was written -- and the
    // axis still runs at the wrong rate. Only the sampled ":j1" check can see
    // that, which is why it exists.
    //
    // The signal asserted here is the check's OWN WARN. Stop counts cannot
    // isolate it: dispatch_goto_locked() stops the RA axis before every goto
    // and refine_goto_landing() re-gotos up to three more times, so
    // stop_count(1) has already moved several times before the check runs.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<bool> condemned{false};
    std::atomic<bool> gave_up{false};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level != alpacacore::logging::LogLevel::Warn) {
                return;
            }
            if (message.find("Post-slew tracking restart") != std::string_view::npos) {
                condemned.store(true);
            }
            if (message.find("restart did not correct it") != std::string_view::npos) {
                gave_up.store(true);
            }
        });

    // The restart's ":J" latches at 2x the commanded rate.
    mount.restart_tracking_at_wrong_rate(1, 1);

    const double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 20.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));

    // The check saw it and recovered on the first attempt: the WARN fired,
    // the "did not correct it" second-attempt WARN did not, and the axis is
    // left running.
    CHECK(condemned.load());
    CHECK_FALSE(gave_up.load());
    REQUIRE(mount.axis_running(1));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the rate check recovers on a mount that decelerates slowly", "[skywatcher][async]") {
    // The same recovery as the case above, on a mount whose ":K" leaves the
    // axis ramping for 800 ms, so the stop/restart spans a real deceleration
    // rather than an instant stop.
    //
    // What this does NOT pin: wait_axis_stationary_locked() itself. In this
    // fake, stop_axis_and_wait_locked() already waits for ":f" to report
    // stopped and the fake stops advancing counts at that same moment, so
    // there is no window for the stationary check to close and the case
    // passes with it deleted (verified). A ramped ":K" cannot open one
    // either: it keeps ":f" RUNNING for the whole ramp, which the ordinary
    // stop-wait already covers. The window the stationary check exists for
    // is the GOTO LANDING, and it is pinned by its own case below
    // ("a goto landing that reports stopped early"), through the
    // land_short_by() seam.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    mount.set_stop_ramp_ms(800);
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<bool> condemned{false};
    std::atomic<bool> gave_up{false};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level != alpacacore::logging::LogLevel::Warn) {
                return;
            }
            if (message.find("Post-slew tracking restart") != std::string_view::npos) {
                condemned.store(true);
            }
            if (message.find("restart did not correct it") != std::string_view::npos) {
                gave_up.store(true);
            }
        });

    mount.restart_tracking_at_wrong_rate(1, 1);

    const double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 22.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));

    // Recovered despite the ramp.
    CHECK(condemned.load());
    CHECK_FALSE(gave_up.load());
    REQUIRE(mount.axis_running(1));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the rate check reports a restart that did not take", "[skywatcher][async]") {
    // Round-1 review finding on #448: the second attempt of the rate check was
    // unreachable, so a restart that ALSO latched wrong was never re-verified
    // and the "restart did not correct it" WARN could never be emitted. The
    // axis then ran at 2x for the rest of the session with no further signal
    // -- the exact #432 symptom the check exists to surface, arrived at from
    // inside the check.
    //
    // Mechanism: entry_generation was captured once before the loop, and the
    // attempt-0 recovery bumps motion_generation_ twice (its own
    // ++motion_generation_ and again inside start_speed_motion_locked()), so
    // attempt 1's first sleep_unlocked() saw a mismatch, read it as "another
    // command owns the axis" and returned immediately.
    //
    // Two bad latches instead of one: the restore's ":J" and the check's own
    // restart both come up at 2x.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<int> condemned{0};
    std::atomic<bool> gave_up{false};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level != alpacacore::logging::LogLevel::Warn) {
                return;
            }
            if (message.find("Post-slew tracking restart") != std::string_view::npos) {
                condemned.fetch_add(1);
            }
            if (message.find("restart did not correct it") != std::string_view::npos) {
                gave_up.store(true);
            }
        });

    mount.restart_tracking_at_wrong_rate(1, 2);

    const double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 24.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));

    // Both attempts ran, and the second one said so. Without the re-seed the
    // loop exits after attempt 0 and gave_up can never become true.
    CHECK(condemned.load() >= 2);
    CHECK(gave_up.load());

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the goto aim-ahead is measured, not the seeded constants", "[skywatcher][async]") {
    // Round-3 review finding on #448: replacing kGotoRampSeconds (2.5 s) and
    // kTrackingResumeSeconds (0.7 s) with measured EMAs
    // (goto_overhead_seconds_, resume_latency_seconds_) is a behaviour change
    // that nothing pinned -- delete both update blocks, re-seed from the
    // constants, and the suite stayed green. That is exactly the failure mode
    // this PR's own rule in .github/instructions/skywatcher.instructions.md warns about.
    //
    // The estimates are private, so the observable is what they steer: the
    // landing residual in RA, which is pure aim-ahead error (Dec has no time
    // term and comes out exactly 0 every slew). Run the same slew shape
    // repeatedly and watch the spread. Measured, the estimate walks as the EMA
    // takes in each landing and the residual walks with it: ~9.3 arcsec of
    // spread over five slews, stable to +/-0.1 across runs. Frozen at the
    // constants it is ~0.13 arcsec -- the residual barely moves, because
    // nothing is adapting. A 2 arcsec floor sits a factor of four below the
    // live value and fifteen above the frozen one.
    //
    // What this case does NOT claim is that the measured estimate lands the
    // mount BETTER. On this fake it does not: frozen residuals run about
    // -0.8 arcsec and measured ones about -3 to -13, because the fake has no
    // equivalent of the real MC's ~3 s floor on even a 350-count goto, which
    // is the whole reason the constants were wrong on an EQM-35. The evidence
    // that measuring helps is the hardware ConformU run in this PR
    // ("SlewToCoordinates 10.8 arc seconds away" -> clean), not this file.
    // Pinned here: that the aim-ahead is driven by something that moves.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // Residual in arcsec of RA, signed, measured once the slew has fully
    // finished (Slewing covers the restore, so the read lands after it).
    const auto landing_residual_arcsec = [&](int i) {
        const double lst = driver->get_sidereal_time();
        const double target_ra = std::fmod(lst - 0.15 + 24.0, 24.0);
        const double target_dec = 20.0 + 2.0 * i;
        driver->slew_to_coordinates_async(target_ra, target_dec);
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));
        CHECK(std::abs(driver->get_declination() - target_dec) < 0.01);
        return (driver->get_right_ascension() - target_ra) * 15.0 * 3600.0;
    };

    double lowest = 1e9;
    double highest = -1e9;
    for (int i = 0; i < 5; ++i) {
        const double residual = landing_residual_arcsec(i);
        lowest = std::min(lowest, residual);
        highest = std::max(highest, residual);
    }

    INFO("landing residual spread over five slews: " << (highest - lowest) << " arcsec");
    CHECK((highest - lowest) > 2.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a rate write during the post-slew restore is applied", "[skywatcher][async]") {
    // Round-2 review finding on #448: holding goto_in_progress_ across the
    // restore made Slewing stay true (which is the point) but ALSO made
    // axes_busy_locked() true, and the rate setters read that as "a goto owns
    // the axes, its restore will re-apply this when it releases them". By
    // then the restore had already run, and nothing re-applies after
    // goto_in_progress_ clears: the write returned 200, RightAscensionRate
    // read back the new value, and the axis kept the old rate for good.
    // Before this PR the flag was already false during the restore, so the
    // same write applied -- a regression introduced by the Slewing fix.
    // restoring_tracking_ now carries the Slewing half on its own.
    //
    // Landing the write INSIDE the window has to be deterministic, and simply
    // writing once Slewing is true is not: most of a slew is the refinement
    // loop, where the axes really are busy and the restore that follows does
    // re-apply the rate, so the write takes effect either way (measured).
    // The one anchor that is inside the window and nowhere else is the rate
    // check's own recovery: arm ONE wrong latch, so the restore's ":J" comes
    // up at 2x, attempt 0 condemns it ("Post-slew tracking restart"), and the
    // recovery stops and restarts the axis -- correctly this time, the knob
    // being spent. The write goes in the moment that restart's ":J" lands,
    // which is the top of attempt 1's ~450 ms sample, with mutex_ released
    // and the restore long since finished.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    const auto ra_travel = [&] {
        const double p0 = mount.physical_degrees(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return std::abs(mount.physical_degrees(1) - p0);
    };
    const double plain_travel = ra_travel();
    REQUIRE(plain_travel > 0.0);

    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<bool> condemned{false};
    std::atomic<int> starts_at_warn{0};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn &&
                message.find("Post-slew tracking restart") != std::string_view::npos && !condemned.load()) {
                // The fake's own mutex, not the driver's: safe to take here.
                starts_at_warn.store(mount.start_count(1));
                condemned.store(true);
            }
        });

    mount.restart_tracking_at_wrong_rate(1, 1);
    const double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 30.0);

    // Attempt 0 condemned the restore, then its recovery's restart went out.
    REQUIRE(wait_until([&] { return condemned.load(); }, 60000));
    REQUIRE(wait_until([&] { return mount.start_count(1) > starts_at_warn.load(); }, 20000));
    REQUIRE(driver->get_slewing());  // the Slewing half of the contract still holds

    driver->set_right_ascension_rate(driver->get_right_ascension_rate() + 0.5);
    REQUIRE(driver->get_right_ascension_rate() == 0.5);

    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // +0.5 s/s slows the drive. Stranded, the axis keeps the rate the restore
    // left it at and travels exactly as far as it did before the slew.
    const double offset_travel = ra_travel();
    INFO("plain travel " << plain_travel << " deg, after a +0.5 s/s write in the restore window " << offset_travel
                         << " deg");
    CHECK(offset_travel < plain_travel);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a goto landing that reports stopped early is waited out", "[skywatcher][async]") {
    // Round-1 review finding on #448: wait_axis_stationary_locked() is the
    // headline change and nothing in the suite failed without it. This case
    // closes that, and it needs a seam the fake did not have -- ":f" clearing
    // its running bit while the last counts are still arriving
    // (land_short_by()), which is what the #432 session measured on the
    // EQM-35 Pro. A ramped ":K" cannot stand in: it keeps ":f" RUNNING for
    // the whole ramp, so the driver's ordinary stop-wait already covers it
    // and the stationary check has no window left to close. That is why the
    // slow-deceleration case above still says it does not pin this.
    //
    // The signal is the check's OWN WARN, and it is deterministic. Goto
    // counts cannot be the signal: refine_goto_landing() burns all three
    // iterations on this fake whether or not a landing coasts (measured:
    // 4 Dec gotos either way), so the count is saturated before the coast
    // can move it. Wall-clock timing cannot be the signal either -- the 3 s
    // slew_force_until_ window and the tracking restore both sit between the
    // landing and Slewing clearing, and either swamps a coast short enough
    // to be waited out.
    //
    // So: coast for longer than kLandingSettleTimeout (2 s). The stationary
    // check gives up and says so, in a string nothing else in the driver
    // emits. Delete the call and the WARN cannot appear.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<bool> still_moving{false};
    alpacacore::logging::set_log_sink([&](alpacacore::logging::LogLevel level, std::string_view,
                                          std::string_view message) {
        if (level == alpacacore::logging::LogLevel::Warn && message.find("still moving ") != std::string_view::npos &&
            message.find(" s after the controller reported it stopped") != std::string_view::npos) {
            still_moving.store(true);
        }
    });

    // Control first: an ordinary landing must NOT trip the check.
    double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 26.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));
    CHECK_FALSE(still_moving.load());

    // Now land the Dec axis 0.05 deg short and creep it in over 3 s, past
    // the 2 s the check is willing to wait.
    mount.land_short_by(2, 20, 0.05, 3000);
    lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 28.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 90000));
    CHECK(still_moving.load());

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - the synchronous slew reports Slewing until tracking is restored", "[skywatcher][async]") {
    // Review finding on PR #448: the async task was reordered to clear
    // goto_in_progress_ AFTER restore_tracking_after_slew_locked(), but the
    // synchronous slew_to_coordinates() still cleared it before. Since the
    // restore now releases mutex_ for the rate check (450 ms, and seconds
    // more if the retry fires), a client polling Slewing saw the slew finish
    // and could fire MoveAxis/PulseGuide into the restart window.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // Force the slow path: the restart latches at the wrong rate, so the
    // check must run its sample window and then stop and restart the axis.
    // All of that has to happen while Slewing is still true.
    mount.restart_tracking_at_wrong_rate(1, 1);

    // The slew runs on its own thread so this one can watch Slewing while the
    // synchronous call is still inside restore_tracking_after_slew_locked().
    //
    // open-astro#537: the original shape had a defect in each direction.
    //
    // FALSE FAILURE. The driver legitimately clears Slewing as the call
    // returns, and the slewer published its completion flag only afterwards.
    // The poller tested that flag and read Slewing as two separate operations,
    // so it could interleave between them, read "not slewing" with the flag
    // still unset, and score a correct driver as broken. Fixed by stamping
    // when the call actually returned and requiring that a false reading
    // COMPLETED before that instant to count: the value get_slewing() returns
    // describes some instant no later than the moment the call completed, so a
    // read completing before the return provably observed a pre-return state,
    // while one completing after it proves nothing either way.
    //
    // VACUOUS PASS. The gating wait accepted EITHER Slewing going true or the
    // slew finishing, so a slew that completed before the first poll satisfied
    // it by completion, the loop body never ran, and the case passed having
    // asserted nothing about the invariant it exists to protect -- the #512 /
    // #514 shape. Fixed by gating on Slewing alone and pairing the negative
    // check with a positive "this actually ran" assertion, which is the remedy
    // #334 already landed for the stress harness (StressCallGuard::total_calls,
    // documented in AGENTS.md as one of three lines that must always appear
    // together). "Never polled" is a failure here, not a pass.
    std::atomic<bool> slew_returned{false};
    std::atomic<std::chrono::steady_clock::rep> returned_at_tick{0};
    const double lst = driver->get_sidereal_time();
    std::thread slewer([&] {
        driver->slew_to_coordinates(std::fmod(lst - 0.15 + 24.0, 24.0), 18.0);
        returned_at_tick.store(std::chrono::steady_clock::now().time_since_epoch().count());
        slew_returned.store(true);
    });

    // Gate on Slewing alone: a slew that finished before we looked leaves this
    // case unable to say anything, which is a failure rather than a pass.
    // Share ONE counter between the gate and the polling loop below (PR review,
    // 2026-09-18): two independent "did we see Slewing true" signals left a
    // window where the gate's read counted but a fast-finishing slew skipped
    // the loop's own first read entirely, so saw_slewing_true could read 0
    // despite the gate having genuinely observed Slewing true moments earlier.
    std::atomic<int> saw_slewing_true{0};
    const bool observed_slewing = wait_until(
        [&] {
            const bool slewing = driver->get_slewing();
            if (slewing) {
                ++saw_slewing_true;
            }
            return slewing;
        },
        5000);

    bool saw_slewing_false = false;
    std::chrono::steady_clock::time_point false_read_completed{};
    while (!slew_returned.load()) {
        const bool slewing = driver->get_slewing();
        if (slewing) {
            ++saw_slewing_true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // Timestamp AFTER the read: the observation it reports happened at or
        // before this instant, so comparing it against the return stamp is
        // conservative in the direction that matters (it can only ever fail to
        // report a violation, never invent one).
        false_read_completed = std::chrono::steady_clock::now();
        saw_slewing_false = true;
        break;
    }
    slewer.join();
    const auto returned_at =
        std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(returned_at_tick.load()));

    // Positive: the invariant was actually exercised.
    REQUIRE(observed_slewing);
    CHECK(saw_slewing_true > 0);
    // Negative: Slewing may only go false once the synchronous call has
    // returned, by which point tracking is restored.
    const bool violated = saw_slewing_false && false_read_completed < returned_at;
    CHECK_FALSE(violated);
    REQUIRE_FALSE(driver->get_slewing());

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher async - a near-cancelled RA rate is not condemned by the post-slew check",
          "[skywatcher][async]") {
    // Review finding on PR #448: observed_cps is a whole-count delta over a
    // fixed window judged against a fixed 25% tolerance, so when the
    // effective RA rate nearly cancels sidereal -- RightAscensionRate = 0.9,
    // the documented satellite/geostationary use -- the window expects ~1.5
    // counts and ":j1" can only answer 1 or 2. BOTH are outside the
    // tolerance, so the check condemned an axis that was tracking correctly:
    // a stop, a restart, a second failed sample and a "restart did not
    // correct it" WARN after every slew for the rest of the session.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    driver->set_right_ascension_rate(0.9);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

    // Counting stops cannot isolate this: refine_goto_landing's re-gotos each
    // stop the axis too. The rate check's own verdict is what matters, so
    // capture its WARN -- it is emitted only when the check decides the axis
    // is running at the wrong rate.
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::atomic<bool> condemned{false};
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn &&
                message.find("Post-slew tracking restart") != std::string_view::npos) {
                condemned.store(true);
            }
        });

    const double lst = driver->get_sidereal_time();
    driver->slew_to_coordinates_async(std::fmod(lst - 0.15 + 24.0, 24.0), 21.0);
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 60000));

    // The axis is healthy, so the check must leave it alone entirely.
    CHECK_FALSE(condemned.load());
    REQUIRE(mount.axis_running(1));

    driver->set_right_ascension_rate(0.0);
    driver->set_tracking(false);
    driver->set_connected(false);
}

#endif  // _WIN32
