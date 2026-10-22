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

// open-astro#437: goto backlash compensation in the Sky-Watcher direct driver,
// a fixed final approach: a goto whose move runs against the axis' approach
// side (RA: the tracking direction of the site's hemisphere; Dec: increasing
// counts) overshoots by the backlash steps and comes back onto the target in a
// slow final leg. The position register is never re-based.
//
// Over the loopback fake mount (EQM-35 Pro profile: its ":s" worm count is a
// hardware capture). The board's frames are read from the fake's ordered motion
// log. Sky gotos compare only Dec frames across runs (the RA targets move with
// the host clock); FindHome gotos go to fixed counts, so both axes compare.
// The fake's gear gap (set_gear_gap) separates the motor, which the register
// counts, from the output shaft, which the sky sees.
//
// Scope the cases pin (decision recorded in
// .github/instructions/skywatcher.instructions.md and SUPPORTED-DRIVERS.md):
// gotos are compensated; PulseGuide and MoveAxis are not.

#ifndef _WIN32

#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/catalog/device_catalog.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"
#include "skywatcher_fake_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
using alpacacore::test::FakeSkyWatcherMount;
using alpacacore::test::FakeTaskClock;
using Frames = std::vector<std::string>;

namespace {

// Site privacy: the public landmarks only.
struct Site {
    double latitude;
    double longitude;
};
constexpr Site kSouth{-33.856784, 151.215297};
constexpr Site kNorth{51.477833, -0.001500};
constexpr double kElevation = 42.0;
constexpr uint32_t kBacklash = 300;
constexpr uint32_t kCountsMask = 0xFFFFFF;
constexpr uint32_t kHomeCounts = 0x800000;

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

sw::BacklashSettings dec_backlash(uint32_t steps, bool enabled = true) {
    sw::BacklashSettings b;
    b.dec = {steps, enabled};
    return b;
}

sw::BacklashSettings ra_backlash(uint32_t steps) {
    sw::BacklashSettings b;
    b.ra = {steps, true};
    return b;
}

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(
    const FakeSkyWatcherMount& mount, const sw::BacklashSettings& backlash, Site site = kSouth,
    alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock()) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), site.latitude, site.longitude, kElevation, {}, {},
                                                  clock, sw::DecAxisSenseSetting::Auto, backlash);
    driver->set_connected(true);
    return driver;
}

// ":S2xxxxxx" / ":I2xxxxxx": the 24-bit payload, nibble-swapped.
uint32_t payload(const std::string& frame) {
    const auto nib = [](char c) { return static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'A' + 10); };
    const std::string d = frame.substr(3, 6);
    return (nib(d[0]) << 4 | nib(d[1])) | (nib(d[2]) << 4 | nib(d[3])) << 8 | (nib(d[4]) << 4 | nib(d[5])) << 16;
}

Frames axis_frames(const Frames& all, int axis) {
    Frames out;
    const char c = static_cast<char>('0' + axis);
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [c](const std::string& f) { return f[2] == c; });
    return out;
}

Frames axis_two(const Frames& all) { return axis_frames(all, 2); }

// A final leg is a slow GOTO (":G<axis>2<dir>"); nothing else the driver sends
// uses that mode. A ":E" would be a register re-base, which the final-approach
// model never writes.
bool is_final_leg(const std::string& f) { return f.rfind(":G", 0) == 0 && f[3] == '2'; }
bool has_final_leg(const Frames& frames) { return std::any_of(frames.begin(), frames.end(), is_final_leg); }

// Goto-jump pumping: skywatcher_fake_clock.h (open-astro#715, one home since #1221).
using alpacacore::test::skywatcher_clock::follow_wall_clock;
using alpacacore::test::skywatcher_clock::pump_until;

void pump_until_landed(FakeTaskClock& clock, alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount) {
    REQUIRE(pump_until(clock, mount, [&] { return !driver.get_slewing(); }, std::chrono::seconds(60)));
}

void land(FakeTaskClock& clock, alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount, double ra,
          double dec) {
    driver.slew_to_coordinates_async(ra, dec);
    pump_until_landed(clock, driver, mount);
}

bool wait_for(const std::function<bool()>& done, std::chrono::seconds bound = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

double counts_deg(uint32_t counts, const FakeMountProfile& profile) { return counts * 360.0 / profile.cpr; }

// For a driver on the fake's own clock (open-astro#1221): steps virtual time
// 250 ms at a time until pred() holds, each step once the driver's task (a
// slew body or a pulse) and its status poll are both parked on the clock, so
// the landing settle, the hold for the sky, the post-slew rate check and a
// pulse's hold all pass in virtual time. False when `limit` of clock time
// passes first, or when no task parks within 2 s of real time and pred() has
// not turned true meanwhile.
bool step_until(FakeTaskClock& clock, const std::function<bool()>& pred,
                std::chrono::seconds limit = std::chrono::seconds(60)) {
    constexpr std::size_t kParked = 2;
    constexpr std::chrono::milliseconds kStep{250};
    constexpr std::chrono::milliseconds kRendezvous{2000};
    for (std::chrono::milliseconds t{0}; t < limit; t += kStep) {
        // A parked task, or pred() turning true while the woken one reacts.
        const auto give_up = std::chrono::steady_clock::now() + kRendezvous;
        while (!clock.wait_for_waiters(kParked, std::chrono::milliseconds(20))) {
            if (pred()) return true;
            if (std::chrono::steady_clock::now() >= give_up) return pred();
        }
        if (pred()) return true;
        clock.advance(kStep);
        if (!clock.wait_for_woken_settled(kRendezvous)) return pred();
    }
    return pred();
}

// A sky goto with the driver on the fake's own clock, stepped until Slewing
// clears.
void land_stepped(FakeTaskClock& clock, alpacacore::TelescopeDriver& driver, double ra, double dec) {
    driver.slew_to_coordinates_async(ra, dec);
    REQUIRE(step_until(clock, [&] { return !driver.get_slewing(); }));
}

// Moves virtual time by `d` in one step for a driver on the fake's clock with
// no task running, and lets the status poll it wakes settle.
void jump_idle(FakeTaskClock& clock, std::chrono::milliseconds d) {
    clock.advance(d);
    REQUIRE(clock.wait_for_woken_settled(std::chrono::milliseconds(2000)));
}

// A fake mount on a fake clock and a driver connected to it, on the wall clock
// or, with `shared_clock`, on the fake's own. Disconnects at the end.
struct Rig {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit Rig(const sw::BacklashSettings& backlash, const FakeMountProfile& profile = FakeMountProfile::eqm35_pro(),
                 Site site = kSouth, bool shared_clock = false)
        : mount(profile, clock) {
        REQUIRE(mount.ok());
        driver = shared_clock ? make_driver(mount, backlash, site, clock) : make_driver(mount, backlash, site);
    }
    ~Rig() { driver->set_connected(false); }
};

// ── FindHome runs: a goto to fixed counts from a chosen start ─────────────

struct HomeRun {
    Frames frames[2];  // per axis (index 0 = RA)
    double register_deg[2] = {0.0, 0.0};
};

// One fresh mount and driver: the axes jumped to `start_deg`, then FindHome
// (no index sensors on this profile: a goto to the count origin).
HomeRun home_run(const FakeMountProfile& profile, const sw::BacklashSettings& backlash, Site site,
                 const double (&start_deg)[2]) {
    Rig rig(backlash, profile, site);
    rig.mount.jump_axis_degrees(1, start_deg[0]);
    rig.mount.jump_axis_degrees(2, start_deg[1]);
    rig.mount.clear_motion_log();
    rig.driver->find_home();
    pump_until_landed(rig.clock, *rig.driver, rig.mount);
    HomeRun r;
    for (int axis = 1; axis <= 2; ++axis) {
        r.frames[axis - 1] = axis_frames(rig.mount.motion_log(), axis);
        r.register_deg[axis - 1] = rig.mount.axis_degrees(axis);
    }
    return r;
}

// The gear-gap cases: register minus shaft at each landing, compensated
// (`on`) and not. Compensated, each landing is within one gap of the shaft and
// engages the gears the same way; uncompensated, they engage both ways, which
// shows the fake's gap is real.
void check_gap_landings(const std::vector<double>& on, const std::vector<double>& off, double gap_deg) {
    for (std::size_t i = 0; i < on.size(); ++i) {
        INFO("landing " << i << ": register minus shaft " << on[i] / gap_deg << " gaps");
        CHECK(std::abs(on[i]) <= gap_deg + 1e-9);
        CHECK(std::abs(on[i] - on[0]) <= gap_deg / 4.0);
    }
    const auto [lo, hi] = std::minmax_element(off.begin(), off.end());
    CHECK(*hi - *lo > gap_deg / 2.0);
}

std::size_t first_target(const Frames& frames) {
    const auto it =
        std::find_if(frames.begin(), frames.end(), [](const std::string& f) { return f.rfind(":S", 0) == 0; });
    return static_cast<std::size_t>(it - frames.begin());
}

// ── Sky goto runs (Dec compared) ───────────────────────────────────────────

struct Run {
    std::vector<Frames> dec_frames;  // per goto, Dec axis only
    std::vector<double> dec_moves;   // per goto, register change in degrees
    double dec_counts_deg = 0.0;     // register angle after the last goto
    int worm_inquiries = 0;
    int position_writes = 0;
};

Run run(const FakeMountProfile& profile, const sw::BacklashSettings& backlash, const std::vector<double>& decs) {
    Rig rig(backlash, profile, kSouth, /*shared_clock=*/true);
    auto& [clock, mount, driver] = rig;
    driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = driver->get_sidereal_time();
    Run result;
    for (const double dec : decs) {
        mount.clear_motion_log();
        const double before = mount.axis_degrees(2);
        land_stepped(clock, *driver, ra, dec);
        result.dec_frames.push_back(axis_two(mount.motion_log()));
        result.dec_moves.push_back(mount.axis_degrees(2) - before);
    }
    result.dec_counts_deg = mount.axis_degrees(2);
    result.worm_inquiries = mount.frames_seen('s');
    result.position_writes = mount.frames_seen('E');
    return result;
}

// Down and up again: whichever way the sky maps onto the Dec counts, one of
// the moves after the first runs against the approach side and one follows it.
const std::vector<double> kBackAndForth{-80.0, -79.0, -79.5, -79.0};

// Each distinct (board, settings) run is made once and shared by the cases
// that read it.
Run cached_run(const std::string& name, const FakeMountProfile& profile, const sw::BacklashSettings& backlash) {
    static std::map<std::string, Run> runs;
    auto it = runs.find(name);
    if (it == runs.end()) {
        it = runs.emplace(name, run(profile, backlash, kBackAndForth)).first;
    }
    return it->second;
}

// A Dec goto against the approach side (FindHome from Dec +4 degrees: down to
// the count origin), held in its first leg: the fake's clock is frozen.
struct MidFirstLeg : Rig {
    double gap_deg = counts_deg(kBacklash, FakeMountProfile::eqm35_pro());

    MidFirstLeg() : Rig(dec_backlash(kBacklash)) {
        mount.set_gear_gap(2, kBacklash);
        mount.jump_axis_degrees(2, 4.0);
        mount.clear_motion_log();
        driver->find_home();
        REQUIRE(wait_for([&] { return mount.axis_in_goto(2); }));
        const Frames first = axis_two(mount.motion_log());
        REQUIRE(first_target(first) < first.size());
        // The first leg overshoots the count origin by the slack.
        CHECK(payload(first[first_target(first)]) == kHomeCounts - kBacklash);
    }
    bool final_leg_sent() { return has_final_leg(axis_two(mount.motion_log())); }
    // Register minus shaft: within one gap whenever the register was left alone.
    double register_error_deg() { return mount.axis_degrees(2) - mount.shaft_degrees(2); }
};

// The same FindHome with the driver on the fake's own clock: run() steps it
// 250 ms at a time, each step once the driver's two waits (the slew body and
// the status poll) are parked, so the landing wait polls once per step. On
// construction the first leg is on its way and has been polled once.
struct SteppedHome : Rig {
    static constexpr std::size_t kParked = 2;
    static constexpr std::chrono::milliseconds kStep{250};
    std::string failure;  // a failed FindHome reports its error through Slewing (open-astro#631)

    explicit SteppedHome(uint32_t steps = kBacklash)
        : Rig(dec_backlash(steps), FakeMountProfile::eqm35_pro(), kSouth, /*shared_clock=*/true) {
        mount.jump_axis_degrees(2, 4.0);
        mount.clear_motion_log();
        driver->find_home();
        REQUIRE(wait_for([&] { return mount.start_count(2) >= 1; }));
        REQUIRE(clock.wait_for_waiters(kParked, std::chrono::milliseconds(2000)));
    }
    bool slewing() {
        try {
            return driver->get_slewing();
        } catch (const alpacacore::AlpacaException& e) {
            failure = e.what();
            return false;
        }
    }
    // Steps the clock until Slewing clears, for at most `limit` of clock time;
    // `home_while_moving` notes an AtHome read while the Dec axis still runs.
    bool home_while_moving = false;
    void run(std::chrono::seconds limit = std::chrono::seconds(30)) {
        for (std::chrono::milliseconds t{0}; t < limit && slewing(); t += kStep) {
            if (!clock.wait_for_waiters(kParked, std::chrono::milliseconds(2000))) return;
            clock.advance(kStep);
            if (!clock.wait_for_woken_settled(std::chrono::milliseconds(2000))) return;
            home_while_moving = home_while_moving || (driver->get_at_home() && mount.axis_running(2));
        }
    }
};

}  // namespace

TEST_CASE("SkyWatcher backlash - a goto against the approach side ends with a slow final leg onto the target",
          "[skywatcher][telescope][backlash]") {
    // Both hemispheres (the RA approach side follows the tracking direction)
    // and both ways round; FindHome goes to the count origin, so the targets
    // and the frames of both axes compare across runs.
    const auto profile = FakeMountProfile::eqm35_pro();
    sw::BacklashSettings both = dec_backlash(kBacklash);
    both.ra = {kBacklash, true};
    // Speed: ':s' (68266 microsteps per worm) at a sixteenth of a turn per
    // second = 4266.6 counts/s against the 16 MHz timer.
    const uint32_t period = static_cast<uint32_t>(std::lround(16000000.0 / (68266.0 / 16.0)));
    int final_legs = 0;
    for (const Site site : {kSouth, kNorth}) {
        const int approach[2] = {site.latitude < 0.0 ? -1 : 1, 1};
        for (const double sign : {1.0, -1.0}) {
            const double start[2] = {4.0 * sign, 4.0 * sign};
            const HomeRun plain = home_run(profile, {}, site, start);
            const HomeRun comp = home_run(profile, both, site, start);
            for (int i = 0; i < 2; ++i) {
                INFO("latitude " << site.latitude << ", start " << start[i] << ", axis " << i + 1);
                const Frames& off = plain.frames[i];
                const Frames& on = comp.frames[i];
                CHECK(comp.register_deg[i] == plain.register_deg[i]);  // lands on the target
                const int move = start[i] > 0.0 ? -1 : 1;              // the goto's way: back to the origin
                if (move == approach[i]) {
                    CHECK(on == off);  // the approach side already: today's commands
                    continue;
                }
                ++final_legs;
                const std::size_t s = first_target(off);
                REQUIRE(s + 1 < off.size());
                REQUIRE(on.size() == off.size() + 5);
                CHECK(Frames(on.begin(), on.begin() + static_cast<std::ptrdiff_t>(s)) ==
                      Frames(off.begin(), off.begin() + static_cast<std::ptrdiff_t>(s)));
                // First leg: the same fast goto, past the target by the slack.
                const uint32_t target = payload(off[s]);
                CHECK(payload(on[s]) == ((target - static_cast<uint32_t>(approach[i]) * kBacklash) & kCountsMask));
                CHECK(on[s + 1] == off[s + 1]);  // ":J"
                // Final leg, through the goto dispatch: its stop, then a slow
                // GOTO in the approach direction, at the derived speed, onto
                // the unchanged target.
                const char ch = static_cast<char>('0' + i + 1);
                CHECK(on[s + 2] == std::string(":K") + ch);
                CHECK(on[s + 3] == std::string(":G") + ch + '2' + (approach[i] > 0 ? '0' : '1'));
                REQUIRE(on[s + 4].rfind(std::string(":I") + ch, 0) == 0);
                CHECK(payload(on[s + 4]) == period);
                CHECK(on[s + 5] == off[s]);  // ":S" target, byte for byte
                CHECK(on[s + 6] == off[s + 1]);
                CHECK(Frames(on.begin() + static_cast<std::ptrdiff_t>(s) + 7, on.end()) ==
                      Frames(off.begin() + static_cast<std::ptrdiff_t>(s) + 2, off.end()));
                CHECK_FALSE(std::any_of(on.begin(), on.end(), [](const std::string& f) { return f[1] == 'E'; }));
            }
        }
    }
    CHECK(final_legs == 4);  // one per axis per hemisphere
}

TEST_CASE("SkyWatcher backlash - without a site the RA axis is not compensated (#1474)",
          "[skywatcher][telescope][backlash]") {
    // Decision 0019: Park and the no-indexer FindHome work without a site. The
    // RA approach side is the tracking direction of the site's hemisphere, and
    // an unset latitude would read as north: a southern rig would load the
    // gears the wrong way. So until both coordinates are known the RA axis
    // sends the plain goto (no overshoot, no final leg); Dec, whose approach
    // side does not depend on the site, is still compensated.
    const auto profile = FakeMountProfile::eqm35_pro();
    sw::BacklashSettings both = dec_backlash(kBacklash);
    both.ra = {kBacklash, true};
    const auto no_site_run = [&](const sw::BacklashSettings& backlash) {
        FakeTaskClock clock;
        FakeSkyWatcherMount mount(profile, clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), std::nullopt, std::nullopt, std::nullopt, {},
                                                      {}, alpacacore::util::default_task_clock(),
                                                      sw::DecAxisSenseSetting::Auto, backlash);
        driver->set_connected(true);
        // Both axes start at +4 degrees: the goto home moves against the north
        // approach side (counts up) that an unset latitude would pick for RA.
        mount.jump_axis_degrees(1, 4.0);
        mount.jump_axis_degrees(2, 4.0);
        mount.clear_motion_log();
        driver->find_home();
        pump_until_landed(clock, *driver, mount);
        HomeRun r;
        for (int axis = 1; axis <= 2; ++axis) {
            r.frames[axis - 1] = axis_frames(mount.motion_log(), axis);
            r.register_deg[axis - 1] = mount.axis_degrees(axis);
        }
        driver->set_connected(false);
        return r;
    };
    const HomeRun plain = no_site_run({});
    const HomeRun comp = no_site_run(both);
    CHECK_FALSE(has_final_leg(comp.frames[0]));
    CHECK(comp.frames[0] == plain.frames[0]);  // RA: today's commands
    CHECK(comp.register_deg[0] == plain.register_deg[0]);
    // Dec +4 down to the origin runs against its approach side: still compensated.
    CHECK(has_final_leg(comp.frames[1]));
    CHECK(comp.frames[1].size() == plain.frames[1].size() + 5);
    CHECK(comp.register_deg[1] == plain.register_deg[1]);
}

TEST_CASE("SkyWatcher backlash - the final leg's speed follows the board's worm count",
          "[skywatcher][telescope][backlash]") {
    auto half = FakeMountProfile::eqm35_pro();
    half.steps_per_worm = *half.steps_per_worm / 2;  // half the worm steps: half the speed
    const double start[2] = {0.0, 4.0};
    const HomeRun a = home_run(FakeMountProfile::eqm35_pro(), dec_backlash(kBacklash), kSouth, start);
    const HomeRun b = home_run(half, dec_backlash(kBacklash), kSouth, start);
    const auto final_period = [](const Frames& frames) {
        const auto it = std::find_if(frames.begin(), frames.end(), is_final_leg);
        REQUIRE(it != frames.end());
        REQUIRE(it + 1 != frames.end());
        return static_cast<double>(payload(*(it + 1)));
    };
    CHECK(std::abs(final_period(b.frames[1]) / final_period(a.frames[1]) - 2.0) < 0.001);
}

TEST_CASE("SkyWatcher backlash - a goto in the approach direction sends the same commands as today",
          "[skywatcher][telescope][backlash]") {
    const auto profile = FakeMountProfile::eqm35_pro();
    const Run plain = cached_run("plain", profile, {});
    const Run comp = cached_run("comp", profile, dec_backlash(kBacklash));
    int approach_gotos_after_a_reversal = 0;
    for (std::size_t i = 0; i < comp.dec_moves.size(); ++i) {
        INFO("goto " << i);
        if (comp.dec_moves[i] > 0.0) {  // Dec approach side: increasing counts
            CHECK(comp.dec_frames[i] == plain.dec_frames[i]);
            CHECK_FALSE(has_final_leg(comp.dec_frames[i]));
            if (i > 0 && comp.dec_moves[i - 1] < 0.0) {
                ++approach_gotos_after_a_reversal;
            }
        } else {
            CHECK(has_final_leg(comp.dec_frames[i]));
        }
    }
    CHECK(approach_gotos_after_a_reversal >= 1);  // the case is not vacuous
    CHECK(comp.dec_counts_deg == plain.dec_counts_deg);
    CHECK(comp.position_writes == 0);
}

TEST_CASE("SkyWatcher backlash - off, or zero steps, sends the same commands as today and never asks ':s'",
          "[skywatcher][telescope][backlash]") {
    const auto profile = FakeMountProfile::eqm35_pro();
    const Run plain = cached_run("plain", profile, {});
    const Run disabled = cached_run("disabled", profile, dec_backlash(kBacklash, false));
    const Run zero = cached_run("zero", profile, dec_backlash(0, true));
    for (const Run* r : {&disabled, &zero}) {
        CHECK(r->dec_frames == plain.dec_frames);
        CHECK(r->worm_inquiries == 0);
    }
}

TEST_CASE("SkyWatcher backlash - a board with no worm count is left uncompensated",
          "[skywatcher][telescope][backlash]") {
    // The Wave 100i rejects ':s' ("!0"); the EQ-AL55i answers 0. Neither gives a
    // speed to derive, so the axis is not compensated.
    auto zero = FakeMountProfile::eqm35_pro();
    zero.steps_per_worm = 0;
    const Run rejected = cached_run("wave", FakeMountProfile::wave_100i(), dec_backlash(kBacklash));
    const Run answered_zero = cached_run("zero-worm", zero, dec_backlash(kBacklash));
    for (const Run* r : {&rejected, &answered_zero}) {
        for (const Frames& frames : r->dec_frames) CHECK_FALSE(has_final_leg(frames));
        CHECK(r->worm_inquiries == 1);
        CHECK(r->position_writes == 0);
    }
}

TEST_CASE("SkyWatcher backlash - a goto that moves the axis by under one count sends no final leg",
          "[skywatcher][telescope][backlash]") {
    // The third goto asks for where the second landed: a zero-count move, whose
    // sign carries no direction.
    const Run r = run(FakeMountProfile::eqm35_pro(), dec_backlash(kBacklash), {-80.0, -79.0, -79.0});
    CHECK_FALSE(has_final_leg(r.dec_frames[2]));
}

TEST_CASE("SkyWatcher backlash - guide pulses and MoveAxis reversals send no final leg",
          "[skywatcher][telescope][backlash]") {
    FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    sw::BacklashSettings both = dec_backlash(kBacklash);
    both.ra = {kBacklash, true};
    auto driver = make_driver(mount, both);
    mount.clear_motion_log();
    driver->pulse_guide(0, 200);  // north, then south: a Dec reversal
    while (driver->get_is_pulse_guiding()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    driver->pulse_guide(1, 200);
    while (driver->get_is_pulse_guiding()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    driver->move_axis(1, 0.5);
    driver->move_axis(1, -0.5);
    driver->move_axis(1, 0.0);
    // No goto target, no position re-base, no slow-GOTO mode on either axis.
    for (const std::string& f : mount.motion_log()) {
        INFO(f);
        CHECK(f.rfind(":S", 0) != 0);
        CHECK(f.rfind(":E", 0) != 0);
        CHECK_FALSE(is_final_leg(f));
    }
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher backlash - RA landings with tracking on stay within one gear gap of the shaft",
          "[skywatcher][telescope][backlash]") {
    // Three cycles of [goto one way, track past the gap, goto back, track]:
    // each cycle has one goto against the tracking direction. The register is
    // never re-based, so it stays within one gap of the shaft; the fixed
    // approach side makes every landing engage the gears the same way.
    constexpr uint32_t kGap = 200;
    const auto profile = FakeMountProfile::eqm35_pro();
    const double gap_deg = counts_deg(kGap, profile);
    const auto landings = [&](const sw::BacklashSettings& backlash) {
        Rig rig(backlash, profile, kSouth, /*shared_clock=*/true);
        auto& [clock, mount, driver] = rig;
        mount.set_gear_gap(1, kGap);
        driver->set_tracking(true);
        jump_idle(clock, std::chrono::seconds(5));  // tracking takes up the gap
        const double lst = driver->get_sidereal_time();
        std::vector<double> errors;  // register minus shaft at each landing
        for (int cycle = 0; cycle < 3; ++cycle) {
            for (const double hours : {0.5, 1.0}) {
                land_stepped(clock, *driver, std::fmod(lst + hours, 24.0), -80.0);
                errors.push_back(mount.axis_degrees(1) - mount.shaft_degrees(1));
                jump_idle(clock, std::chrono::seconds(5));  // let tracking run past the gap
            }
        }
        return errors;
    };
    const std::vector<double> on = landings(ra_backlash(kGap));
    check_gap_landings(on, landings({}), gap_deg);
}

TEST_CASE("SkyWatcher backlash - Dec landings after guide pulses stay within one gear gap of the shaft",
          "[skywatcher][telescope][backlash]") {
    // Three cycles; before each goto a guide pulse moves Dec the other way, so
    // the pulse ends opposite to the goto. 40 counts of gap: a 1.5 s pulse at
    // the default guide rate (about 53 counts/s here) crosses it.
    constexpr uint32_t kGap = 40;
    const auto profile = FakeMountProfile::eqm35_pro();
    const double gap_deg = counts_deg(kGap, profile);
    const auto landings = [&](const sw::BacklashSettings& backlash, double* pulse_moved_shaft) {
        Rig rig(backlash, profile, kSouth, /*shared_clock=*/true);
        auto& [clock, mount, driver] = rig;
        mount.set_gear_gap(2, kGap);
        driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
        const double ra = driver->get_sidereal_time();
        const auto pulse = [&](int direction) {
            const double before = mount.axis_degrees(2);
            const double shaft_before = mount.shaft_degrees(2);
            driver->pulse_guide(direction, 1500);
            REQUIRE(step_until(clock, [&] { return !driver->get_is_pulse_guiding(); }, std::chrono::seconds(10)));
            *pulse_moved_shaft = std::max(*pulse_moved_shaft, std::abs(mount.shaft_degrees(2) - shaft_before));
            return mount.axis_degrees(2) - before;
        };
        land_stepped(clock, *driver, ra, -80.0);
        const double north_way = pulse(0) > 0.0 ? 1.0 : -1.0;  // register direction of a north pulse
        std::vector<double> errors;
        double from = mount.axis_degrees(2);
        land_stepped(clock, *driver, ra, -79.0);
        const double up_way = mount.axis_degrees(2) > from ? 1.0 : -1.0;  // register direction of a higher Dec
        for (int cycle = 0; cycle < 3; ++cycle) {
            for (const double dec : {-79.5, -79.0}) {
                const double goto_way = dec > -79.25 ? up_way : -up_way;
                pulse(north_way == goto_way ? 1 : 0);  // the pulse ends opposite to the goto
                land_stepped(clock, *driver, ra, dec);
                errors.push_back(mount.axis_degrees(2) - mount.shaft_degrees(2));
            }
        }
        return errors;
    };
    double moved_on = 0.0;
    double moved_off = 0.0;
    const std::vector<double> on = landings(dec_backlash(kGap), &moved_on);
    check_gap_landings(on, landings({}, &moved_off), gap_deg);
    CHECK(moved_off > 0.0);  // the pulses crossed the gap
}

TEST_CASE("SkyWatcher backlash - an AbortSlew during the first leg stops it and sends no final leg",
          "[skywatcher][telescope][backlash]") {
    MidFirstLeg rig;
    rig.driver->abort_slew();
    CHECK_FALSE(rig.mount.axis_running(2));  // stopped at once, the clock still frozen
    REQUIRE(wait_for([&] { return !rig.driver->get_slewing(); }));
    rig.clock.advance(std::chrono::seconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // a wait poll or two
    CHECK_FALSE(rig.final_leg_sent());
    CHECK(rig.mount.frames_seen('E') == 0);
    CHECK(std::abs(rig.register_error_deg()) <= rig.gap_deg + 1e-9);
}

TEST_CASE("SkyWatcher backlash - a slew wait that times out with a final leg owed sends no final leg",
          "[skywatcher][telescope][backlash]") {
    // The slew-complete wait gives up at 180 s of clock time while the board
    // never reports the first leg done.
    SteppedHome rig;
    rig.mount.hold_running(2, true);
    rig.run(std::chrono::seconds(200));
    CHECK(rig.failure.find("Slew timed out") != std::string::npos);  // the wait gave up
    CHECK_FALSE(rig.driver->get_at_home());
    rig.mount.hold_running(2, false);
    CHECK_FALSE(has_final_leg(axis_two(rig.mount.motion_log())));
    CHECK(rig.mount.frames_seen('E') == 0);
}

TEST_CASE("SkyWatcher backlash - a second goto during the first leg lands on its own target",
          "[skywatcher][telescope][backlash]") {
    MidFirstLeg rig;
    rig.driver->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    rig.mount.clear_motion_log();
    // Three hours west of the meridian: on this board and site that is the
    // Dec branch whose axis angle for Dec -70 is +20 degrees, so the second
    // goto runs up, in the approach direction, and owes no final leg of its own.
    const double before = rig.mount.axis_degrees(2);
    rig.driver->slew_to_coordinates_async(std::fmod(rig.driver->get_sidereal_time() + 21.0, 24.0), -70.0);
    pump_until_landed(rig.clock, *rig.driver, rig.mount);
    const Frames frames = axis_two(rig.mount.motion_log());
    REQUIRE(rig.mount.axis_degrees(2) > before);
    CHECK_FALSE(has_final_leg(frames));
    // Every Dec target after the second dispatch is the second goto's, none
    // the first goto's origin: its unsent final leg died with its generation.
    std::vector<uint32_t> targets;
    for (const std::string& f : frames) {
        if (f.rfind(":S", 0) == 0) targets.push_back(payload(f));
    }
    REQUIRE_FALSE(targets.empty());
    for (const uint32_t t : targets) CHECK(t != kHomeCounts);
    const double landed = rig.mount.axis_degrees(2);
    CHECK(std::abs(landed - (static_cast<double>(targets.back()) - kHomeCounts) * 360.0 /
                                FakeMountProfile::eqm35_pro().cpr) < 1e-9);
    CHECK(rig.mount.frames_seen('E') == 0);
    CHECK(std::abs(rig.register_error_deg()) <= rig.gap_deg + 1e-9);
}

TEST_CASE("SkyWatcher backlash - a MoveAxis during the first leg sends no final leg",
          "[skywatcher][telescope][backlash]") {
    MidFirstLeg rig;
    rig.driver->move_axis(1, 0.5);  // Dec: the goto's wait loses the axis to the manual motion
    follow_wall_clock(rig.clock, std::chrono::milliseconds(300));
    rig.driver->move_axis(1, 0.0);
    REQUIRE(wait_for([&] {
        rig.clock.advance(std::chrono::milliseconds(100));
        return !rig.mount.axis_running(2);
    }));
    // The FindHome's landing wait now finds both axes at rest: give it a few
    // polls to send whatever it would. (Not Slewing: FindHome cut short by a
    // MoveAxis leaves Slewing set, a separate defect this case does not pin.)
    follow_wall_clock(rig.clock, std::chrono::milliseconds(1500));
    CHECK_FALSE(rig.final_leg_sent());
    CHECK(rig.mount.frames_seen('E') == 0);
    CHECK(std::abs(rig.register_error_deg()) <= rig.gap_deg + 1e-9);
}

TEST_CASE("SkyWatcher backlash - the final leg waits until the first leg has stopped moving",
          "[skywatcher][telescope][backlash]") {
    // An EQM-35 Pro reported a goto stopped while its last counts were still
    // arriving (kLandingSettle in the driver). The first leg lands 0.05 degrees
    // short with ':f' already reading stopped and creeps in the rest over
    // 600 ms, inside the landing settle's 2 s: no command reaches the axis
    // until it is still.
    SteppedHome rig;
    rig.mount.land_short_by(2, 1, 0.05, 600);
    rig.run();
    CHECK(rig.failure.empty());
    CHECK(rig.mount.commands_while_coasting(2) == 0);
    CHECK(has_final_leg(axis_two(rig.mount.motion_log())));
    CHECK(rig.mount.axis_degrees(2) == 0.0);  // the target: the count origin
}

TEST_CASE("SkyWatcher backlash - a failed status read during either leg does not fail the goto",
          "[skywatcher][telescope][backlash]") {
    // The landing wait keeps its last answer across a status read that fails
    // (get_hardware_slewing_locked(), for up to 10 s): one failed ':f' on the
    // Dec axis during the first leg is no error, and three from the final
    // leg's start (a 20000-count leg, about 4.7 s at the derived speed) do not
    // land it early. The status poll is paused, so the reads that fail are the
    // landing wait's.
    for (const bool final_leg : {false, true}) {
        INFO((final_leg ? "from the final leg's start" : "during the first leg"));
        SteppedHome rig(final_leg ? 20000 : kBacklash);
        sw::set_status_poll_paused_for_testing(*rig.driver, true);
        rig.mount.fail_status_replies(2, final_leg ? 3 : 1, /*after_next_start=*/final_leg);
        rig.run();
        CHECK(rig.mount.status_failures_pending(2) == 0);  // the failed replies were served
        CHECK(rig.failure.empty());
        CHECK_FALSE(rig.home_while_moving);
        CHECK(rig.driver->get_at_home());
        CHECK(has_final_leg(axis_two(rig.mount.motion_log())));
        CHECK(rig.mount.axis_degrees(2) == 0.0);
    }
}

TEST_CASE("SkyWatcher backlash - the catalog keys default to off, are range-checked and reach the driver",
          "[skywatcher][telescope][backlash][catalog]") {
    using namespace alpacacore::catalog;
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    register_builtin_factories(catalog);
    const DeviceKey key{"skywatcher", alpacacore::DeviceType::Telescope};
    constexpr std::int64_t kStepsMax = 100000;

    FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    DeviceConfig config;
    config.set("connectionType", std::string{"network"});
    config.set("host", std::string{"127.0.0.1"});
    config.set("udpPort", static_cast<std::int64_t>(mount.port()));
    config.set("siteLatitude", kSouth.latitude);
    config.set("siteLongitude", kSouth.longitude);

    // A config without the keys loads and behaves as today: no ':s' read and
    // no final leg through a Dec goto either way.
    const auto absent = catalog.normalize(key, config, Source::Api);
    REQUIRE_FALSE(absent.rejection.has_value());
    {
        auto idle = catalog.create(key, absent.config, 0);
        auto* t = dynamic_cast<alpacacore::TelescopeDriver*>(idle.get());
        REQUIRE(t != nullptr);
        t->set_connected(true);
        t->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
        const double ra0 = t->get_sidereal_time();
        mount.clear_motion_log();
        for (const double dec : kBackAndForth) land(clock, *t, mount, ra0, dec);
        CHECK(mount.frames_seen('s') == 0);
        CHECK_FALSE(has_final_leg(mount.motion_log()));
        t->set_connected(false);
    }
    const int worm_before = mount.frames_seen('s');

    // Out of range is refused, in range kept as written.
    for (const char* name : {"backlashStepsRA", "backlashStepsDec"}) {
        for (const std::int64_t bad : {std::int64_t{-1}, kStepsMax + 1}) {
            DeviceConfig c = config;
            c.set(name, bad);
            INFO(name << " " << bad);
            CHECK(catalog.normalize(key, c, Source::Api).rejection.has_value());
        }
    }
    config.set("backlashStepsRA", kStepsMax);
    config.set("backlashEnabledRA", true);
    config.set("backlashStepsDec", static_cast<std::int64_t>(kBacklash));
    config.set("backlashEnabledDec", true);
    const auto saved = catalog.normalize(key, config, Source::Api);
    REQUIRE_FALSE(saved.rejection.has_value());
    // The four values survive the round trip as written.
    const auto int_at = [&](const char* k) {
        const ConfigValue* v = saved.config.find_value(k);
        REQUIRE(v != nullptr);
        return std::get<std::int64_t>(*v);
    };
    const auto bool_at = [&](const char* k) {
        const ConfigValue* v = saved.config.find_value(k);
        REQUIRE(v != nullptr);
        return std::get<bool>(*v);
    };
    CHECK(int_at("backlashStepsRA") == kStepsMax);
    CHECK(bool_at("backlashEnabledRA"));
    CHECK(int_at("backlashStepsDec") == static_cast<std::int64_t>(kBacklash));
    CHECK(bool_at("backlashEnabledDec"));

    // The factory hands them to the driver: a Dec goto against the approach
    // side now ends with a final leg.
    auto driver = catalog.create(key, saved.config, 0);
    auto* telescope = dynamic_cast<alpacacore::TelescopeDriver*>(driver.get());
    REQUIRE(telescope != nullptr);
    telescope->set_connected(true);
    telescope->set_tracking(true);  // equatorial slews need Tracking (open-astro#1274)
    const double ra = telescope->get_sidereal_time();
    mount.clear_motion_log();
    for (const double dec : kBackAndForth) land(clock, *telescope, mount, ra, dec);
    CHECK(mount.frames_seen('s') == worm_before + 2);  // both axes asked for a worm count
    CHECK(has_final_leg(axis_two(mount.motion_log())));
    CHECK(mount.frames_seen('E') == 0);
    telescope->set_connected(false);
}

#endif
