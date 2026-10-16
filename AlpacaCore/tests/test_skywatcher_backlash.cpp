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

// open-astro#437: goto backlash compensation in the Sky-Watcher direct driver.
//
// Over the loopback fake mount (EQM-35 Pro profile: its ":s" worm count is a
// hardware capture). The board's frames are read from the fake's ordered motion
// log. Only the Dec axis' frames are compared across runs: the RA targets move
// with the host clock, the Dec targets do not.
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

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
using alpacacore::test::FakeSkyWatcherMount;
using Frames = std::vector<std::string>;

namespace {

constexpr double kLatitude = -33.856784;
constexpr double kLongitude = 151.215297;
constexpr double kElevation = 42.0;
constexpr uint32_t kBacklash = 300;

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

// ":S2xxxxxx" / ":E2xxxxxx" / ":I2xxxxxx": the 24-bit payload, nibble-swapped.
uint32_t payload(const std::string& frame) {
    const auto nib = [](char c) { return static_cast<uint32_t>(c <= '9' ? c - '0' : c - 'A' + 10); };
    const std::string d = frame.substr(3, 6);
    return (nib(d[0]) << 4 | nib(d[1])) | (nib(d[2]) << 4 | nib(d[3])) << 8 | (nib(d[4]) << 4 | nib(d[5])) << 16;
}

Frames axis_two(const Frames& all) {
    Frames out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [](const std::string& f) { return f[2] == '2'; });
    return out;
}

// The fake integrates motion against a FakeTaskClock (the driver stays on the
// wall clock): while an axis runs a GOTO, jump virtual time so it lands at once;
// otherwise let it follow wall time. The rendezvous is the board's next frame.
// Under util::kLinkStalenessBound (30 s) per jump.
void pump_until_landed(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                       FakeSkyWatcherMount& mount) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    auto last = std::chrono::steady_clock::now();
    while (driver.get_slewing()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        if (mount.axis_in_goto(1) || mount.axis_in_goto(2)) {
            clock.advance(std::chrono::seconds(20));
        }
        mount.wait_for_frames(0, mount.frames_seen() + 1, std::chrono::milliseconds(50));
        const auto t = std::chrono::steady_clock::now();
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(t - last));
        last = t;
    }
}

void land(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount,
          double ra, double dec) {
    driver.slew_to_coordinates_async(ra, dec);
    pump_until_landed(clock, driver, mount);
}

Frames axis_one(const Frames& all) {
    Frames out;
    std::copy_if(all.begin(), all.end(), std::back_inserter(out), [](const std::string& f) { return f[2] == '1'; });
    return out;
}

bool wait_for(const std::function<bool()>& done, std::chrono::seconds bound = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool has_takeup(const Frames& frames) {
    return std::any_of(frames.begin(), frames.end(), [](const std::string& f) {
        return f.rfind(":E", 0) == 0 || (f.rfind(":G", 0) == 0 && f[3] == '2');
    });
}

// A mount with Dec backlash on, Dec carried upward by two gotos, then a third
// (reversing) goto started and held part-way through its take-up: the fake's
// clock is frozen, so the slack is closed by `partial_s` virtual seconds only
// (the fake slews at 3.3 deg/s: 0.0015 s covers under half of a 300-count slack).
struct MidTakeUp {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount{FakeMountProfile::eqm35_pro(), clock};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    double ra = 0.0;
    double before_deg = 0.0;  // register angle before the reversing goto
    double moved_deg = 0.0;   // how far the register ran ahead during the take-up
    double partial_s;

    // `start_goto` false: the caller issues the reversing goto itself.
    explicit MidTakeUp(bool start_goto = true, double partial = 0.0015) : partial_s(partial) {
        REQUIRE(mount.ok());
        driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, {},
                                                 alpacacore::util::default_task_clock(), sw::DecAxisSenseSetting::Auto,
                                                 dec_backlash(kBacklash));
        driver->set_connected(true);
        ra = driver->get_sidereal_time();
        land(clock, *driver, mount, ra, -80.0);
        land(clock, *driver, mount, ra, -79.0);
        before_deg = mount.axis_degrees(2);
        mount.clear_motion_log();
        if (start_goto) {
            driver->slew_to_coordinates_async(ra, -79.5);
            run_partial_takeup();
        }
    }
    // Wait for the take-up to start, then let it run `partial_s` virtual seconds.
    // Returns false when the take-up never started or never moved the register
    // (no Catch2 assertion here: the timeout case calls it from a helper thread).
    bool partial_takeup_ran() {
        if (!wait_for([&] { return mount.axis_in_goto(2) && has_takeup(axis_two(mount.motion_log())); })) {
            return false;
        }
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(partial_s)));
        moved_deg = mount.axis_degrees(2) - before_deg;
        return std::abs(moved_deg) > 0.0;  // the register is ahead of the shaft
    }
    void run_partial_takeup() { REQUIRE(partial_takeup_ran()); }
    ~MidTakeUp() { driver->set_connected(false); }
};

struct Run {
    std::vector<Frames> dec_frames;  // per goto, Dec axis only
    double dec_counts_deg = 0.0;     // register angle after the last goto
    double dec_physical_deg = 0.0;   // where the shaft really is
    int worm_inquiries = 0;
    int position_writes = 0;
};

// One fresh mount, one fresh driver, one goto per entry of `decs` (all at the
// same RA, so the Dec axis is the only one that reverses).
Run run(const FakeMountProfile& profile, const sw::BacklashSettings& backlash, const std::vector<double>& decs) {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(profile, clock);
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, {},
                                                  alpacacore::util::default_task_clock(), sw::DecAxisSenseSetting::Auto,
                                                  backlash);
    driver->set_connected(true);
    const double ra = driver->get_sidereal_time();
    Run result;
    for (const double dec : decs) {
        mount.clear_motion_log();
        land(clock, *driver, mount, ra, dec);
        result.dec_frames.push_back(axis_two(mount.motion_log()));
    }
    result.dec_counts_deg = mount.axis_degrees(2);
    result.dec_physical_deg = mount.physical_degrees(2);
    result.worm_inquiries = mount.frames_seen('s');
    result.position_writes = mount.frames_seen('E');
    driver->set_connected(false);
    return result;
}

const std::vector<double> kReversal{-80.0, -79.0, -79.5};

// A slew is real time on the loopback fake, so each distinct (board, settings)
// run is made once and shared by the cases that read it.
Run cached_run(const std::string& name, const FakeMountProfile& profile, const sw::BacklashSettings& backlash) {
    static std::map<std::string, Run> runs;
    auto it = runs.find(name);
    if (it == runs.end()) {
        it = runs.emplace(name, run(profile, backlash, kReversal)).first;
    }
    return it->second;
}

// The second goto continues the first one's way; the third turns Dec round.

}  // namespace

TEST_CASE("SkyWatcher backlash - a reversing goto takes up the slack first and lands on the same target",
          "[skywatcher][telescope][backlash]") {
    const auto profile = FakeMountProfile::eqm35_pro();
    const Run plain = cached_run("plain", profile, {});
    const Run comp = cached_run("comp", profile, dec_backlash(kBacklash));

    // The reversing goto: stop, then take-up (slow GOTO ":G2" + period + target
    // + start), then ":E" re-basing the register, then the unchanged real move.
    const Frames& on = comp.dec_frames[2];
    const Frames& off = plain.dec_frames[2];
    REQUIRE(on.size() == off.size() + 5);
    CHECK(on[0] == off[0]);  // ":K2"
    REQUIRE(on[1].rfind(":G22", 0) == 0);
    REQUIRE(on[2].rfind(":I2", 0) == 0);
    REQUIRE(on[3].rfind(":S2", 0) == 0);
    REQUIRE(on[4].rfind(":J2", 0) == 0);
    REQUIRE(on[5].rfind(":E2", 0) == 0);
    CHECK(Frames(on.begin() + 6, on.end()) == Frames(off.begin() + 1, off.end()));

    // Direction: the real move's, the opposite of the previous goto's.
    const char real_dir = off[1][4];
    CHECK(on[1][4] == real_dir);
    CHECK(on[1][4] != plain.dec_frames[1][1][4]);

    // Magnitude: exactly `kBacklash` counts from where the axis was, and the
    // register is put back on that count afterwards.
    const uint32_t origin = payload(on[5]);
    const int64_t takeup = static_cast<int64_t>(payload(on[3])) - static_cast<int64_t>(origin);
    CHECK(takeup == (real_dir == '0' ? 1 : -1) * static_cast<int64_t>(kBacklash));

    // Speed: ':s' (68266 microsteps per worm) at a sixteenth of a turn per
    // second = 4266.6 counts/s against the 16 MHz timer.
    CHECK(payload(on[2]) == static_cast<uint32_t>(std::lround(16000000.0 / (68266.0 / 16.0))));

    // Same target in the controller's frame; the shaft moved the slack extra.
    CHECK(comp.dec_counts_deg == plain.dec_counts_deg);
    const double slack_deg = kBacklash * 360.0 / profile.cpr;
    CHECK(std::abs(std::abs(comp.dec_physical_deg - plain.dec_physical_deg) - slack_deg) < 1e-9);
    CHECK(comp.worm_inquiries == 1);
}

TEST_CASE("SkyWatcher backlash - the take-up speed follows the board's worm count",
          "[skywatcher][telescope][backlash]") {
    auto fast = FakeMountProfile::eqm35_pro();
    fast.steps_per_worm = *fast.steps_per_worm / 2;  // half the worm steps: half the speed
    const Run a = cached_run("comp", FakeMountProfile::eqm35_pro(), dec_backlash(kBacklash));
    const Run b = cached_run("half-worm", fast, dec_backlash(kBacklash));
    REQUIRE(a.dec_frames[2].size() > 2);
    REQUIRE(b.dec_frames[2].size() > 2);
    const double period_a = payload(a.dec_frames[2][2]);
    const double period_b = payload(b.dec_frames[2][2]);
    CHECK(std::abs(period_b / period_a - 2.0) < 0.001);
}

TEST_CASE("SkyWatcher backlash - a goto that does not reverse sends the same commands as today",
          "[skywatcher][telescope][backlash]") {
    const auto profile = FakeMountProfile::eqm35_pro();
    const Run plain = cached_run("plain", profile, {});
    const Run comp = cached_run("comp", profile, dec_backlash(kBacklash));
    // The first goto has no history; the second continues its way.
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}}) {
        CHECK(comp.dec_frames[i] == plain.dec_frames[i]);
        CHECK_FALSE(has_takeup(comp.dec_frames[i]));
    }
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
        CHECK(r->dec_physical_deg == plain.dec_physical_deg);
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
        for (const Frames& frames : r->dec_frames) CHECK_FALSE(has_takeup(frames));
        CHECK(r->worm_inquiries == 1);
        CHECK(r->position_writes == 0);
    }
}

TEST_CASE("SkyWatcher backlash - a goto reverses against the axis' last motion, whatever command made it",
          "[skywatcher][telescope][backlash]") {
    // Two gotos carry Dec upward; a MoveAxis in one direction or the other then
    // sets the last direction of travel (and sends no extra counts itself); the
    // next goto, upward again, reverses it in exactly one of the two cases.
    const auto after_move = [](double rate) {
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro(), clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, {},
                                                      alpacacore::util::default_task_clock(),
                                                      sw::DecAxisSenseSetting::Auto, dec_backlash(kBacklash));
        driver->set_connected(true);
        const double ra = driver->get_sidereal_time();
        land(clock, *driver, mount, ra, -80.0);
        land(clock, *driver, mount, ra, -79.0);
        mount.clear_motion_log();
        driver->move_axis(1, rate);
        driver->move_axis(1, 0.0);
        const Frames moved = axis_two(mount.motion_log());
        mount.clear_motion_log();
        land(clock, *driver, mount, ra, -78.0);
        const Frames next = axis_two(mount.motion_log());
        driver->set_connected(false);
        return std::make_pair(has_takeup(moved), has_takeup(next));
    };
    const auto up = after_move(0.5);
    const auto down = after_move(-0.5);
    CHECK_FALSE(up.first);  // MoveAxis itself is never compensated
    CHECK_FALSE(down.first);
    CHECK(up.second != down.second);
}

TEST_CASE("SkyWatcher backlash - a goto that moves the axis by under one count is not a reversal",
          "[skywatcher][telescope][backlash]") {
    // The third goto asks for where the second landed: a zero-count move, whose
    // sign carries no direction.
    const Run r = run(FakeMountProfile::eqm35_pro(), dec_backlash(kBacklash), {-80.0, -79.0, -79.0});
    CHECK_FALSE(has_takeup(r.dec_frames[2]));
}

TEST_CASE("SkyWatcher backlash - guide pulses and MoveAxis reversals send no take-up",
          "[skywatcher][telescope][backlash]") {
    FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    sw::BacklashSettings both = dec_backlash(kBacklash);
    both.ra = {kBacklash, true};
    auto driver =
        sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, {},
                                        alpacacore::util::default_task_clock(), sw::DecAxisSenseSetting::Auto, both);
    driver->set_connected(true);
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
        CHECK_FALSE((f.rfind(":G", 0) == 0 && f[3] == '2'));
    }
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher backlash - an AbortSlew during the take-up leaves the register where the shaft is",
          "[skywatcher][telescope][backlash]") {
    MidTakeUp rig;
    rig.driver->abort_slew();
    REQUIRE(wait_for([&] { return rig.mount.frames_seen('E') >= 1; }));
    CHECK(rig.mount.axis_degrees(2) == rig.before_deg);
}

TEST_CASE("SkyWatcher backlash - a take-up that times out leaves the register where the shaft is",
          "[skywatcher][telescope][backlash]") {
    MidTakeUp rig(false);
    // The clock stays frozen after the partial run: the axis never reports
    // done and the driver's wait (about five wall-clock seconds) gives up. A
    // synchronous goto hands the failure to this thread, so the register is
    // read at the moment the call returns, with no later dispatch to fix it.
    bool ran = false;
    std::thread helper([&] { ran = rig.partial_takeup_ran(); });
    CHECK_THROWS_AS(rig.driver->slew_to_coordinates(rig.ra, -79.5), alpacacore::AlpacaException);
    helper.join();
    REQUIRE(ran);
    CHECK(rig.mount.axis_degrees(2) == rig.before_deg);
}

TEST_CASE("SkyWatcher backlash - a second goto during the take-up lands on its own target",
          "[skywatcher][telescope][backlash]") {
    MidTakeUp rig;
    rig.driver->slew_to_coordinates_async(rig.ra, -70.0);
    pump_until_landed(rig.clock, *rig.driver, rig.mount);
    const Run plain = run(FakeMountProfile::eqm35_pro(), {}, {-80.0, -79.0, -70.0});
    CHECK(rig.mount.axis_degrees(2) == plain.dec_counts_deg);
    // The cut-short take-up moved the shaft; the re-base left that displacement
    // in the frame (an unrestored register would leave none). Same way as the
    // partial run, no further than the slack.
    const double slack_deg = kBacklash * 360.0 / FakeMountProfile::eqm35_pro().cpr;
    const double shaft = rig.mount.physical_degrees(2) - plain.dec_physical_deg;
    CHECK(shaft * rig.moved_deg > 0.0);
    CHECK(std::abs(shaft) <= slack_deg + 1e-9);
}

TEST_CASE("SkyWatcher backlash - a MoveAxis during the take-up leaves the register where the shaft is",
          "[skywatcher][telescope][backlash]") {
    MidTakeUp rig;
    rig.driver->move_axis(1, 0.5);  // the clock is frozen: the axis has not moved yet
    CHECK(rig.mount.axis_degrees(2) == rig.before_deg);
    rig.driver->move_axis(1, 0.0);
    CHECK(rig.mount.axis_degrees(2) == rig.before_deg);
}

// Command letters and modes only: RA targets follow the host clock.
Frames shape(const Frames& frames) {
    Frames out;
    for (const std::string& f : frames) out.push_back(f.substr(0, f.rfind(":G", 0) == 0 ? 5 : 3));
    return out;
}

TEST_CASE("SkyWatcher backlash - a reversing RA goto takes up the slack first and lands on the same target",
          "[skywatcher][telescope][backlash]") {
    struct Result {
        std::vector<Frames> gotos;
        double shaft_before = 0.0;  // physical minus register angle, before the last goto
        double shaft_after = 0.0;
    };
    const auto ra_run = [](const sw::BacklashSettings& backlash) {
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro(), clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kLatitude, kLongitude, kElevation, {}, {},
                                                      alpacacore::util::default_task_clock(),
                                                      sw::DecAxisSenseSetting::Auto, backlash);
        driver->set_connected(true);
        const double lst = driver->get_sidereal_time();
        Result r;
        for (const double hours : {0.5, 1.0, 0.75}) {  // on, on, back
            r.shaft_before = mount.physical_degrees(1) - mount.axis_degrees(1);
            mount.clear_motion_log();
            land(clock, *driver, mount, std::fmod(lst + hours, 24.0), -80.0);
            r.gotos.push_back(axis_one(mount.motion_log()));
            r.shaft_after = mount.physical_degrees(1) - mount.axis_degrees(1);
        }
        driver->set_connected(false);
        return r;
    };
    sw::BacklashSettings ra_only;
    ra_only.ra = {kBacklash, true};
    const Result on = ra_run(ra_only);
    const Result off = ra_run({});

    CHECK_FALSE(has_takeup(on.gotos[1]));  // continues the second goto's way
    const Frames& with = on.gotos[2];
    const Frames& without = off.gotos[2];
    REQUIRE(with.size() == without.size() + 5);
    // Stop, then slow GOTO + period + target + start + re-base, then the
    // unchanged real move (same command letters, modes and directions).
    CHECK(shape(with)[0] == shape(without)[0]);
    REQUIRE(with[1].rfind(":G12", 0) == 0);
    CHECK(with[2].rfind(":I1", 0) == 0);
    REQUIRE(with[3].rfind(":S1", 0) == 0);
    CHECK(with[4].rfind(":J1", 0) == 0);
    REQUIRE(with[5].rfind(":E1", 0) == 0);
    CHECK(shape(Frames(with.begin() + 6, with.end())) == shape(Frames(without.begin() + 1, without.end())));
    // Magnitude and direction of the take-up: the slack, the real move's way.
    const int64_t takeup = static_cast<int64_t>(payload(with[3])) - static_cast<int64_t>(payload(with[5]));
    const char real_dir = without[1][4];
    CHECK(with[1][4] == real_dir);
    CHECK(takeup == (real_dir == '0' ? 1 : -1) * static_cast<int64_t>(kBacklash));
    // The shaft really moved the slack extra, in that direction.
    const double slack_deg = kBacklash * 360.0 / FakeMountProfile::eqm35_pro().cpr;
    CHECK(std::abs(std::abs(on.shaft_after - on.shaft_before) - slack_deg) < 1e-6);
    CHECK(std::abs(off.shaft_after - off.shaft_before) < 1e-6);
}

TEST_CASE("SkyWatcher backlash - the catalog keys default to off, are range-checked and reach the driver",
          "[skywatcher][telescope][backlash][catalog]") {
    using namespace alpacacore::catalog;
    DeviceCatalog catalog;
    register_builtin_schemas(catalog);
    register_builtin_factories(catalog);
    const DeviceKey key{"skywatcher", alpacacore::DeviceType::Telescope};
    constexpr std::int64_t kStepsMax = 100000;

    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    DeviceConfig config;
    config.set("connectionType", std::string{"network"});
    config.set("host", std::string{"127.0.0.1"});
    config.set("udpPort", static_cast<std::int64_t>(mount.port()));
    config.set("siteLatitude", kLatitude);
    config.set("siteLongitude", kLongitude);

    // A config without the keys loads and behaves as today: no ':s' read, no
    // take-up, no re-base through a Dec reversal.
    const auto absent = catalog.normalize(key, config, Source::Api);
    REQUIRE_FALSE(absent.rejection.has_value());
    {
        auto idle = catalog.create(key, absent.config, 0);
        auto* t = dynamic_cast<alpacacore::TelescopeDriver*>(idle.get());
        REQUIRE(t != nullptr);
        t->set_connected(true);
        const double ra0 = t->get_sidereal_time();
        for (const double dec : kReversal) land(clock, *t, mount, ra0, dec);
        CHECK(mount.frames_seen('s') == 0);
        CHECK(mount.frames_seen('E') == 0);
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

    // The factory hands them to the driver: a Dec reversal now takes up.
    auto driver = catalog.create(key, saved.config, 0);
    auto* telescope = dynamic_cast<alpacacore::TelescopeDriver*>(driver.get());
    REQUIRE(telescope != nullptr);
    telescope->set_connected(true);
    const double ra = telescope->get_sidereal_time();
    for (const double dec : kReversal) land(clock, *telescope, mount, ra, dec);
    CHECK(mount.frames_seen('s') == worm_before + 2);  // both axes asked for a worm count
    CHECK(mount.frames_seen('E') >= 1);                // the re-base after a take-up
    telescope->set_connected(false);
}

#endif
