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

// Sky-truth checks for the Sky-Watcher direct driver's pointing model
// (open-astro#432).
//
// WHY THIS FILE EXISTS. Every other SkyWatcher loopback test, and ConformU
// too, judges the driver against its own reported coordinates. The driver
// computes those from the same motor counts it commanded, so it always
// agrees with itself and a wrong pointing model is invisible. That is how a
// six-hour error shipped and passed conformance on three different boards.
//
// WHAT IS AND IS NOT AN EXTERNAL ANCHOR HERE. The hardware rows in the
// first test case are: an EQM-35 Pro at latitude -37.2 was driven to known
// axis positions on 2026-09-12 with the shipped (wrong) 3.5.1 build and the
// tube's real direction was read off the mount by hand (three rows), a
// fourth, northern row comes from the Wave 150i report that opened the
// issue, and the rest are listed in that case. Those rows, the alt/az
// cross-check against what was observed, and the plate-solved rows in the
// last test case ("measured axes agree with the plate-solved sky across a
// flip") are the only checks in this file that the driver cannot satisfy by
// agreeing with itself.
//
// `sky_from_axes()` below is a transcription of the driver's own formula, so
// the goto cases downstream of it pin the goto path against the model rather
// than against the sky. They are still worth having -- they catch a goto that
// stops commanding what the model says -- but they are not independent
// evidence for the model. `sky_from_indi_eqmod()` is (open-astro#458): a
// transcription of a different, field-proven driver, which the model has to
// agree with for a board of the same dec-axis sense.
//
// If you change the pointing model, this file is what has to justify it, and
// a new hardware row, in the first test case or as a plate-solved row in the
// last, is what has to extend it. Do not "verify" a change here
// against the driver's own readback.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

constexpr double kPi = std::numbers::pi;

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

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

double wrap_ha(double hours) {
    double h = std::fmod(hours, 24.0);
    if (h < -12.0) {
        h += 24.0;
    }
    if (h >= 12.0) {
        h -= 24.0;
    }
    return h;
}

struct SkyPoint {
    double ha_hours;
    double dec_degrees;
};

struct AltAz {
    double altitude_degrees;
    double azimuth_degrees;
};

// THE REFERENCE. Where a Sky-Watcher German equatorial physically points for
// raw axis angles measured from the counterweight-down, tube-at-the-pole home.
//
//   HA  = s * (a1 / 15) + s * eps * (a2 >= 0 ? +6 h : -6 h)
//   dec = s * (90 - |a2|)                      s = +1 north, -1 south
//
// The 6 h term is the home position: with the counterweight hanging straight
// down the dec axis lies in the meridian plane, so rotating the dec axis
// alone sweeps the tube along the HA = +/-6 h circle and the meridian is
// reached only with the counterweight bar horizontal. Its sign is the side
// of the meridian the tube swings to, which is fixed by the board's
// dec-axis count sense `eps` (wiring, not latitude) and mirrored by the
// hemisphere like everything else (open-astro#458): the board is never told
// the latitude, and a mount facing the other pole is the same mount turned
// half a turn about the vertical. `board_sense` is eps for a board whose
// sense was measured (hardware rows below), and 0 for an unmeasured one,
// which the driver keeps on the #432 model (s * eps = +1 everywhere).
//
// Caveat (open-astro#459): this oracle reads the branch from the sign of a2
// alone. Inside the driver's two-count deadband of a2 = 0 the driver answers
// the branch it last commanded instead, so a case landing there must judge
// the report against the target, as the pole cases below do, never through
// check_landing().
SkyPoint sky_from_axes(double latitude_degrees, double a1_degrees, double a2_degrees, int board_sense = 0) {
    const double s = latitude_degrees < 0.0 ? -1.0 : 1.0;
    const double home_sign = board_sense == 0 ? 1.0 : s * board_sense;
    const double ha = s * (a1_degrees / 15.0) + home_sign * (a2_degrees >= 0.0 ? 6.0 : -6.0);
    return {wrap_ha(ha), s * (90.0 - std::abs(a2_degrees))};
}

// open-astro#458: an independent second reference. indi-eqmod's
// EncodersToRADec() (indilib/indi-3rdparty @ f2844cb52ce4,
// indi-eqmod/eqmodbase.cpp, with EncoderToHours/EncoderToDegrees and its
// DEStepHome = DEStepInit + steps/4 from skywatcher.cpp) transcribed onto
// this file's a1/a2, rather than onto the driver's formula. Field-proven on
// Synta boards in both hemispheres, and it never asks the board for a sense,
// so it is the eps = +1 case of the reference above.
SkyPoint sky_from_indi_eqmod(double latitude_degrees, double a1_degrees, double a2_degrees) {
    const bool north = latitude_degrees >= 0.0;
    // EncoderToHours(): (initstep - step) / total * 24, i.e. -a1/15 mod 24.
    double hours = std::fmod(-a1_degrees / 15.0 + 48.0, 24.0);
    hours = north ? std::fmod(hours + 6.0, 24.0) : std::fmod((24.0 - hours) + 6.0, 24.0);
    // EncoderToDegrees() on the dec encoder, whose zero is a quarter turn
    // before home: range360(a2 + 90), mirrored south of the equator.
    double deg = std::fmod(a2_degrees + 90.0 + 720.0, 360.0);
    if (!north) {
        deg = std::fmod(360.0 - deg + 360.0, 360.0);
    }
    // EncodersToRADec(): RA = hours + LST, with the 12 h pier correction.
    double ra_minus_lst = hours;
    if (north ? (deg > 90.0 && deg <= 270.0) : (deg <= 90.0 || deg > 270.0)) {
        ra_minus_lst += north ? -12.0 : 12.0;
    }
    // rangeDec(): fold the 0..360 dec encoder angle onto -90..+90.
    double dec = deg;
    if (dec > 270.0) {
        dec -= 360.0;
    } else if (dec > 90.0) {
        dec = 180.0 - dec;
    }
    return {wrap_ha(-ra_minus_lst), dec};
}

AltAz horizon_from_sky(const SkyPoint& p, double latitude_degrees) {
    const double h = p.ha_hours * 15.0 * kPi / 180.0;
    const double d = p.dec_degrees * kPi / 180.0;
    const double l = latitude_degrees * kPi / 180.0;
    const double sin_alt = std::clamp(std::sin(d) * std::sin(l) + std::cos(d) * std::cos(l) * std::cos(h), -1.0, 1.0);
    const double alt = std::asin(sin_alt);
    const double cos_az =
        std::clamp((std::sin(d) - std::sin(alt) * std::sin(l)) / (std::cos(alt) * std::cos(l)), -1.0, 1.0);
    double az = std::acos(cos_az) * 180.0 / kPi;
    if (std::sin(h) > 0.0) {
        az = 360.0 - az;
    }
    return {alt * 180.0 / kPi, az};
}

struct LandedFrame {
    double a1;
    double a2;
    double lst;
    double reported_ra;
    double reported_dec;
    int side_of_pier;
};

// Covers the ~1 s of sidereal motion between the snapshot reads plus the
// goto's own landing deadband (~8 arcsec).
constexpr double kHaToleranceHours = 0.01;
constexpr double kDecToleranceDegrees = 0.05;

void check_landing(const LandedFrame& f, double latitude, double target_ra, double target_dec, int expected_side,
                   int board_sense = 0) {
    const SkyPoint sky = sky_from_axes(latitude, f.a1, f.a2, board_sense);
    const double target_ha = wrap_ha(f.lst - target_ra);
    INFO("axes a1=" << f.a1 << " a2=" << f.a2 << " -> real HA " << sky.ha_hours << " h, dec " << sky.dec_degrees
                    << "; target HA " << target_ha << " h, dec " << target_dec);
    // 1. The tube physically points at the target, judged by the reference.
    CHECK(std::abs(wrap_ha(sky.ha_hours - target_ha)) < kHaToleranceHours);
    CHECK(std::abs(sky.dec_degrees - target_dec) < kDecToleranceDegrees);
    // 2. The driver's own report agrees, which is all the older tests checked.
    CHECK(std::abs(wrap_ha(f.reported_ra - target_ra)) < kHaToleranceHours);
    CHECK(std::abs(f.reported_dec - target_dec) < kDecToleranceDegrees);
    // 3. The counterweight bar never rises above horizontal on a goto.
    CHECK(std::abs(f.a1) <= 90.0 + 0.5);
    // 4. The reported pier side is the ASCOM side for that hour angle.
    CHECK(f.side_of_pier == expected_side);
}

// ── Fake-clock helpers (open-astro#715) ─────────────────────────────────────
//
// The fake integrates motion against a FakeTaskClock; the driver itself stays
// on the wall clock in this slice (the issue's rule), so nothing here waits on
// the clock and FakeTaskClock::wait_for_waiters() has nothing to rendezvous
// with. The rendezvous is the board traffic instead: each step waits for the
// driver's next frame, then decides how virtual time moves:
//   - while an axis runs a goto, jump far enough that it lands at once (a
//     goto's duration is the only thing this suite ever spent real time on);
//   - otherwise let virtual time follow wall time, so a tracking RA axis and
//     the driver's LST agree (decision 0001: LST is host time, never this
//     clock) and the driver's sampled post-slew rate check sees a sidereal
//     axis.
// A jump never moves a tracking axis: only a goto axis is jumped, and the
// driver restarts tracking only after both axes have landed. Nothing here
// sleeps for a driver timer.
constexpr auto kGotoJump = std::chrono::seconds(120);  // > any goto here (180 deg at 800x sidereal = 55 s)
constexpr auto kFrameRendezvous = std::chrono::milliseconds(50);

bool run_until_on_fake_clock(alpacacore::test::FakeTaskClock& clock, FakeSkyWatcherMount& mount,
                             const std::function<bool()>& pred, std::chrono::milliseconds real_bound) {
    const auto deadline = std::chrono::steady_clock::now() + real_bound;
    auto last = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (mount.axis_in_goto(1) || mount.axis_in_goto(2)) {
            clock.advance(kGotoJump);
        }
        mount.wait_for_frames(0, mount.frames_seen() + 1, kFrameRendezvous);
        const auto t = std::chrono::steady_clock::now();
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(t - last));
        last = t;
    }
    return true;
}

LandedFrame land_on_fake_clock(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                               FakeSkyWatcherMount& mount, double ra_hours, double dec) {
    driver.slew_to_coordinates_async(ra_hours, dec);
    REQUIRE(driver.get_slewing());
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver.get_slewing(); }, std::chrono::seconds(60)));
    LandedFrame f{};
    f.lst = driver.get_sidereal_time();
    f.a1 = mount.physical_degrees(1);
    f.a2 = mount.physical_degrees(2);
    f.reported_ra = driver.get_right_ascension();
    f.reported_dec = driver.get_declination();
    f.side_of_pier = driver.get_side_of_pier();
    return f;
}

}  // namespace

TEST_CASE("SkyWatcher pointing - the model reproduces the positions measured on hardware (#432)",
          "[skywatcher][telescope][pointing]") {
    // Rows 1-3: EQM-35 Pro, latitude -37.2 (rounded), 2026-09-12, shipped
    // 3.5.1 build, tube direction read off the mount by hand after each
    // goto. Row 4: the Wave 150i report that opened #432, latitude +45.45.
    // Row 5 (open-astro#458): the same EQM-35 Pro on 2026-09-19 with the
    // 4.0.0 build, still set up facing the south pole but with the driver's
    // latitude at +37.2. The board is never told the latitude, and that
    // mount is a northern one turned half a turn about the vertical, so the
    // row is a northern reading with every azimuth shifted by 180 deg. The
    // driver aimed at HA -3 h, dec +30 (alt 52, az 87) and the saddle ended
    // pointing front-left and slightly down (SE, about -8 deg), which is
    // HA +9 h: the 6 h term had to flip with the hemisphere.
    // What row 5 refutes is the OLD model, which reported the tube 52 deg UP
    // while it pointed below the horizon; the alt tolerance below compares
    // this oracle against itself, so it is not the evidence. No azimuth was
    // read that night (expect_az < 0), unlike rows 1 and 3.
    // Row 6 (open-astro#579): a Sky-Watcher EQ-AL55i Pro (mount code 0x09,
    // firmware 3.46, since updated to 3.48) on the owner's mount, read on
    // 2026-09-20 at about +40 (the site is rounded on purpose). Bare mount at
    // count home, tracking off, then a dec-only MoveAxis raised a2 by 893,663
    // counts = +89.37 deg on the board's own :a2 of 3,600,000. The owner read
    // the DOVETAIL POINTING WEST by eye against the horizon, which is HA +6 h
    // at this latitude, so eps = +1 on this board -- the indi-eqmod sense, and
    // the opposite of the EQM-35 Pro. Unlike rows 1-5 this row constrains the
    // model only in the north, where s * eps = +1 is also the unmeasured
    // default: what it buys is the SOUTH, where the entry now says -1 and the
    // default would be 12 h out. That southern consequence is geometry, not a
    // measurement, exactly as for the Wave 150i.
    // "West by eye" is coarser evidence than rows 1-5's readings, but the two
    // alternatives here are 12 h apart, so it separates them.
    // Read this row's columns for what they are: the DOVETAIL BEARING is the
    // only measured quantity in it. expect_ha, expect_dec, expect_alt and
    // expect_az are this oracle's own output for the reported axis angles, so
    // the alt/az checks below compare the oracle against itself here, exactly
    // as row 5's comment says of its own altitude. What the row pins is that
    // the model, given a2 = +89.37 at +40, puts the tube WEST -- which the
    // owner saw -- rather than 12 h away to the east.
    struct Row {
        const char* what;
        double latitude;
        double a1;
        double a2;
        int board_sense;  // the board's dec-axis count sense, see sky_from_axes()
        double expect_ha;
        double expect_dec;
        double expect_alt;
        double expect_az;  // negative = not read off the mount for this row
        const char* observed;
    };
    const Row rows[] = {
        {"counterweight down, dec axis square", -37.2, 1.6, -90.0, -1, -6.11, 0.0, -1.3, 91.0, "level, pointing east"},
        {"RA axis 60 deg, dec axis square", -37.2, 60.0, -90.0, -1, -10.00, 0.0, -43.6, -1.0, "down about 45 deg"},
        {"RA axis 45 deg, dec axis 70 deg", -37.2, 45.1, -70.0, -1, -9.01, -20.0, -18.9, 136.0,
         "down, azimuth about 136"},
        {"Wave 150i, the #432 report", 45.45, 61.98, 70.95, +1, 10.13, 19.05, -20.7, -1.0, "down about 20 deg"},
        {"EQM-35 Pro at a northern latitude", 37.2, 45.0, -60.0, -1, 9.00, 30.0, -10.7, -1.0,
         "front-left and slightly down on the south-facing rig, i.e. SE, about -8 deg"},
        {"EQ-AL55i Pro, the #579 reading", 40.0, 0.0, 89.37, +1, 6.00, 0.63, 0.41, 270.5,
         "dovetail west, level, after a dec-only move of +89.37 deg from home"},
    };
    for (const Row& r : rows) {
        const SkyPoint sky = sky_from_axes(r.latitude, r.a1, r.a2, r.board_sense);
        const AltAz horizon = horizon_from_sky(sky, r.latitude);
        INFO(r.what << ": observed " << r.observed);
        CHECK(std::abs(wrap_ha(sky.ha_hours - r.expect_ha)) < 0.02);
        CHECK(std::abs(sky.dec_degrees - r.expect_dec) < 0.1);
        CHECK(std::abs(horizon.altitude_degrees - r.expect_alt) < 0.5);
        if (r.expect_az >= 0.0) {
            // A second independent quantity per row where the azimuth was
            // read off the mount as well as the altitude.
            CHECK(std::abs(horizon.azimuth_degrees - r.expect_az) < 1.0);
        }
    }

    // The first row is the one that needs no instrument, and it is what
    // refutes the shipped model on its own: counterweight straight down and
    // the dec axis at 90 deg puts the tube perpendicular to both the polar
    // axis and the counterweight bar, which share one vertical plane, so the
    // tube MUST be level. Level and square to the meridian is six hours of
    // hour angle away from it. The shipped model called that position
    // HA -11.9 h, which is 53 degrees below the horizon.
    const AltAz perpendicular = horizon_from_sky(sky_from_axes(-37.2, 0.0, -90.0), -37.2);
    CHECK(std::abs(perpendicular.altitude_degrees) < 0.5);
}

TEST_CASE("SkyWatcher pointing - a goto west of the meridian lands on the sky, south (#432)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);  // HA +3 h
    const double target_dec = -20.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0, -1);
    // HA >= 0 takes the a2 >= 0 branch here because k = s * eps = +1 for
    // this board in the south (#458); south of the equator the RA axis then
    // runs the other way: a1 = -(3 - 6) * 15 = +45.
    CHECK(f.a2 > 0.0);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto east of the meridian lands on the sky, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = -60.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1, -1);
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a1 + 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

// open-astro#306/#579: the same site and target as the first EQM-35 Pro case
// above, on the EQ-AL55i Pro instead. Its eps is +1 against the EQM-35's -1,
// so south of the equator k = s * eps is -1 rather than +1, and the goto
// reaches the SAME sky target with the dec axis on the mirrored branch:
// a2 = -70 here where the EQM-35 lands a2 = +70. a1 is +45 on both, because
// `branch = k * side` makes k cancel out of the a1 inversion.
//
// What it must NOT change is the pier side. The side is read off the sky hour
// angle in both hemispheres and on every board (driver L2610-2616), so both
// boards answer pierEast (0) here; an earlier draft of this case asserted the
// opposite and the driver was right. The board-dependent quantity is the
// mechanical branch, not the ASCOM side, and that is what is pinned below.
//
// This is also the only loopback coverage of a board whose two axes report
// different counts per revolution (:a1 4,032,000, :a2 3,600,000). The dec
// landing is what catches it: a driver that used the RA figure for the dec
// axis would miss the declination by about 8 degrees.
TEST_CASE("SkyWatcher pointing - the EQ-AL55i Pro reaches a southern target on the mirrored dec branch (#579)",
          "[skywatcher][telescope][pointing][al55i][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eq_al55i(), clock);
    REQUIRE(mount.ok());
    REQUIRE(mount.kCprDec != mount.kCpr);
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);

    // ":e" -> "=032E09": firmware 3.46, mount code 0x09. The name comes from
    // mount_code_to_name(), so this is the only thing that pins `case 0x09`.
    CHECK(driver->get_name() == "Sky-Watcher EQ-AL55i Pro (EQMOD)");
    const auto firmware = driver->get_device_firmware();
    REQUIRE(firmware.has_value());
    CHECK(*firmware == "3.46");

    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);  // HA +3 h
    const double target_dec = -20.0;
    // Board-independent: the EQM-35 Pro answers 0 for this target too.
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0, +1);
    // k = -1 here, so the a2 >= 0 branch would need a1 = -135 and is out of
    // reach; the goto takes the other branch. The EQM-35 Pro case above lands
    // the same a1 with a2 ON THE OTHER SIDE, which is the whole effect of the
    // mount code on this target.
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a2 + 70.0) < 0.2);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

// On the fake clock (open-astro#715): the first case moved off real-rate
// motion, and the shape the rest of the goto cases follow. The assertions
// and tolerances are those of the real-rate version.
TEST_CASE("SkyWatcher pointing - the Wave 150i goto from the #432 report lands on the sky, north",
          "[skywatcher][telescope][pointing][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    const double latitude = 45.45;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 4.12 + 24.0, 24.0);  // HA +4.12 h, the Arcturus geometry
    const double target_dec = 19.05;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0);
    // The shipped build sent a1 = +62 here and the tube ended 20 deg below
    // the horizon. The correct axis angle is (4.12 - 6) * 15 = -28.2.
    CHECK(std::abs(f.a1 + 28.2) < 1.0);
    CHECK(std::abs(f.a2 - 70.95) < 0.2);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto east of the meridian lands on the sky, north",
          "[skywatcher][telescope][pointing][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    const double latitude = 45.45;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = 40.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1);
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - tracking holds the physical hour angle in both hemispheres",
          "[skywatcher][telescope][pointing][hemisphere]") {
    struct Site {
        alpacacore::test::FakeMountProfile profile;
        double latitude;
        double longitude;
        double expected_axis_sign;  // which way the counts must run to follow the sky
    };
    const Site sites[] = {
        {alpacacore::test::FakeMountProfile::wave_100i(), 45.0, 11.0, +1.0},
        {alpacacore::test::FakeMountProfile::eqm35_pro(), -35.0, 150.0, -1.0},
    };
    for (const Site& site : sites) {
        FakeSkyWatcherMount mount(site.profile);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), site.latitude, site.longitude, 100.0);
        driver->set_connected(true);
        mount.jump_axis_degrees(2, 45.0);  // off the pole, where hour angle means something
        driver->set_tracking(true);
        REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

        const double lst0 = driver->get_sidereal_time();
        const double a1_0 = mount.physical_degrees(1);
        const double ra0 = driver->get_right_ascension();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const double lst1 = driver->get_sidereal_time();
        const double a1_1 = mount.physical_degrees(1);
        const double ra1 = driver->get_right_ascension();

        INFO("latitude " << site.latitude << ": axis1 " << a1_0 << " -> " << a1_1 << " deg over "
                         << (lst1 - lst0) * 3600.0 << " sidereal seconds");
        // Counts up north of the equator, down south of it.
        CHECK((a1_1 - a1_0) * site.expected_axis_sign > 0.0);
        // And at the rate that keeps the tube on the star: the physical hour
        // angle advances with sidereal time.
        const double physical_advance = wrap_ha(sky_from_axes(site.latitude, a1_1, 45.0).ha_hours -
                                                sky_from_axes(site.latitude, a1_0, 45.0).ha_hours);
        const double lst_advance = lst1 - lst0;
        CHECK(physical_advance > 0.0);
        CHECK(std::abs(physical_advance - lst_advance) < 0.3 * lst_advance + 0.5 / 3600.0);
        // ...which is the same thing as the reported RA standing still.
        CHECK(std::abs(wrap_ha(ra1 - ra0)) * 3600.0 < 2.0);

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher pointing - an East guide pulse with Tracking off runs the southern way",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Round-2 review finding: pulse_guide()'s not-tracking RA branch is the
    // SECOND call site of ra_axis_sign_locked() and had no case of its own.
    // Every other pulse test connects at latitude +39.7392, where the sign is
    // +1 and the factor is a no-op, and the southern East-pulse case below
    // enables tracking, so it takes the ra_rate_adjust branch instead. Delete
    // `ra_axis_sign_locked() *` from that line and the whole suite stayed
    // green while an autoguider pulsing a parked-rate mount below the equator
    // pushed the star the wrong way.
    //
    // Sky sense, south: guide East = RA increasing. dec = -(90 - |a2|) and
    // HA = -(a1/15) + 6 on this branch, so RA = LST - HA rises when a1 rises:
    // the axis must run in the INCREASING-count direction, the opposite of
    // the same pulse in the north.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    REQUIRE_FALSE(driver->get_tracking());

    const double a1_before = mount.physical_degrees(1);
    driver->pulse_guide(2, 1500);  // East, 1.5 s, no tracking to fold it into
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 8000));
    const double moved = mount.physical_degrees(1) - a1_before;

    INFO("south, Tracking off, East pulse: axis 1 moved " << moved << " deg");
    CHECK(moved > 0.0);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a southern East guide pulse stays on the in-place rate change",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Round-2 review finding, the other unguarded sign: the in-place-versus-
    // stop-and-restart guard is `axis_sign * ra_pulse_rate <= kMinInPlace...`.
    // Drop the axis_sign factor and every southern pulse rate is negative, so
    // the guard is always true and EVERY guide correction takes the full
    // stop-and-restart path -- the mount stops and re-accelerates the RA axis
    // on every cycle of a guiding session. Magnitude and direction both still
    // come out right on that path, which is why the direction case below
    // passes either way; only the stop count can see it.
    //
    // This is the southern twin of the northern assertion in
    // test_skywatcher_async.cpp ("a kick, never a stop/restart").
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // past the ramp
    const int stops_before = mount.stop_count(1);

    driver->pulse_guide(2, 1200);  // East, at the 0.5x default guide rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 8000));

    INFO("south, tracking, East pulse: RA stops " << stops_before << " -> " << mount.stop_count(1));
    CHECK(mount.stop_count(1) == stops_before);
    CHECK(mount.axis_running(1));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an East guide pulse slows the axis in the tracking sense, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Guide East means the tube falls behind the sky, so the RA axis must run
    // SLOWER in whichever direction tracking uses -- below the equator that is
    // the decreasing-count direction. An East correction that sped the axis up
    // or reversed it would push the star the wrong way on every guide cycle.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // past the ramp

    const double sidereal_deg_per_sec = 360.0 / 86164.0905;
    const double a1_before = mount.physical_degrees(1);
    const double ra_before = driver->get_right_ascension();
    const auto t0 = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 2000);  // East, 2 s at the 0.5x default guide rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double moved = mount.physical_degrees(1) - a1_before;
    const double ra_after = driver->get_right_ascension();

    INFO("axis1 moved " << moved * 3600.0 << " arcsec in " << elapsed << " s");
    CHECK(moved < 0.0);  // still turning the tracking way, never reversed
    CHECK(std::abs(moved) < sidereal_deg_per_sec * (elapsed - 0.5));
    CHECK(wrap_ha(ra_after - ra_before) > 0.0);  // an autoguider sees RA rise

    driver->set_tracking(false);
    driver->set_connected(false);
}

// open-astro#459: at the exact pole both dec-axis branches command the same
// encoder count (a2 = branch * (90 - 90) rounds to home either way), so the
// branch cannot be read back from the axis. The driver remembers the branch
// the command path chose and uses it inside a two-count deadband of a2 = 0.
// The oracle above is deliberately left as the pure axis formula: at the pole
// its HA is undefined physically (the tube points at the pole whatever a1
// says), so these cases judge the driver's REPORT against the target rather
// than the oracle.
namespace {
struct PoleCase {
    alpacacore::test::FakeMountProfile profile;
    double latitude;
    double longitude;
    double ha_hours;    // target hour angle; the sign picks the branch
    int expected_side;  // ASCOM side for that hour angle
};
}  // namespace

TEST_CASE("SkyWatcher pointing - a slew to the exact pole reads back the target RA, not 12 h out (#459)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    const PoleCase cases[] = {
        {alpacacore::test::FakeMountProfile::wave_100i(), 45.0, 11.0, -0.1, 1},    // north, east of the meridian
        {alpacacore::test::FakeMountProfile::wave_100i(), 45.0, 11.0, +0.1, 0},    // north, west
        {alpacacore::test::FakeMountProfile::eqm35_pro(), -35.0, 150.0, -0.1, 1},  // south, east
    };
    for (const auto& c : cases) {
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(c.profile, clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), c.latitude, c.longitude, 100.0);
        driver->set_connected(true);
        driver->set_tracking(true);

        const double lst = driver->get_sidereal_time();
        const double target_ra = std::fmod(lst - c.ha_hours + 24.0, 24.0);
        const double target_dec = c.latitude < 0.0 ? -90.0 : 90.0;
        INFO("HA " << c.ha_hours << " h, latitude " << c.latitude);
        REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == c.expected_side);

        const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
        INFO("axes a1=" << f.a1 << " a2=" << f.a2 << " reported RA " << f.reported_ra << " target " << target_ra);
        // The dec axis really is at the pole, inside the two-count deadband
        // where branch_from_axis_locked() consults the memory: the encoder
        // carries no branch, so this case exercises the remembered one.
        CHECK(std::abs(f.a2) <= 2.0 * 360.0 / static_cast<double>(c.profile.cpr));
        // Before #459 the negative-HA rows reported RA 12 h out here.
        CHECK(std::abs(wrap_ha(f.reported_ra - target_ra)) < kHaToleranceHours);
        CHECK(std::abs(f.reported_dec - target_dec) < kDecToleranceDegrees);
        // SideOfPier agrees with the branch the goto chose, by construction.
        CHECK(f.side_of_pier == c.expected_side);

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher pointing - after leaving the pole the branch comes from the axis again (#459)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    const double latitude = 45.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    // Park the remembered branch on the NEGATIVE side with a pole slew ...
    double lst = driver->get_sidereal_time();
    const LandedFrame pole = land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 0.1, 24.0), 90.0);
    REQUIRE(pole.side_of_pier == 1);

    // ... then a goto west of the meridian, which commands the positive
    // branch (a2 > 0). The readback must follow the axis, not the stale
    // memory: the oracle, the report and the pier side all say west.
    lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);  // HA +3 h
    const double target_dec = 20.0;
    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0);
    CHECK(f.a2 > 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a sync at the exact pole reads back the synced RA, not 12 h out (#459)",
          "[skywatcher][telescope][pointing][hemisphere]") {
    // Polar-alignment routines sync at the pole. The sync path writes the
    // same -0.0 dec-axis angle the goto path does on the negative branch, so
    // it has to record the branch too; with that line missing the readback
    // used the stale connect-time branch (+1) and reported RA 12 h out with
    // SideOfPier on the wrong side.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 45.0, 11.0, 100.0);
    driver->set_connected(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 0.1, 24.0);  // HA -0.1 h: the negative branch
    const double target_dec = 90.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    driver->sync_to_coordinates(target_ra, target_dec);

    const double a2 = mount.physical_degrees(2);
    INFO("axis a2=" << a2 << " reported RA " << driver->get_right_ascension() << " target " << target_ra);
    CHECK(std::abs(a2) <= 2.0 * 360.0 / static_cast<double>(alpacacore::test::FakeMountProfile::wave_100i().cpr));
    CHECK(std::abs(wrap_ha(driver->get_right_ascension() - target_ra)) < kHaToleranceHours);
    CHECK(std::abs(driver->get_declination() - target_dec) < kDecToleranceDegrees);
    CHECK(driver->get_side_of_pier() == 1);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - AutoHome resets the remembered branch to the positive side (#459)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // AutoHome ends by stamping the dec axis to the home count, a2 = 0, which
    // is inside the deadband: the reported RA and SideOfPier there come from
    // the remembered branch. The re-anchor resets it to the positive branch,
    // the pre-#459 answer at home; without that reset a FindHome after an
    // east-side goto would still report the east side at the pole.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    mount.set_home_index_degrees(1, 2.0);
    mount.set_home_index_degrees(2, 2.0);
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 45.0, 11.0, 100.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    // Park the memory on the negative branch with an east-of-meridian goto,
    // close to the pole so the AutoHome hunt afterwards is short.
    const double lst = driver->get_sidereal_time();
    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 2.0, 24.0), 85.0);  // HA -2 h
    REQUIRE(f.side_of_pier == 1);
    REQUIRE(f.a2 < -1.0);

    // Tracking off, as the FindHome fixture in test_skywatcher_async.cpp does;
    // the hunt starts 40 degrees from the index, so it takes longer than the
    // from-home run there. The hunt runs in speed mode, not a goto, so the
    // fake-clock wait lets virtual time follow wall time: it stays real-rate.
    driver->set_tracking(false);
    driver->find_home();
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return driver->get_at_home(); }, std::chrono::seconds(180)));
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(5)));

    INFO("after AutoHome a2=" << mount.physical_degrees(2) << " reported RA " << driver->get_right_ascension());
    REQUIRE(std::abs(driver->get_declination() - 90.0) < 0.2);
    CHECK(driver->get_side_of_pier() == 0);
    // The +6 h home term: at home the report is LST - (a1 + 6 h) with a1 = 0.
    CHECK(std::abs(wrap_ha(driver->get_right_ascension() - (driver->get_sidereal_time() - 6.0))) < 0.05);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - the no-indexer FindHome lands on the positive branch (#459)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // A board without home sensors (the EQM-35 Pro profile) homes with a plain
    // goto to a2 = +0.0. That goto has to set the branch memory like any
    // other, or FindHome would answer a different SideOfPier than AutoHome
    // does for the same mechanical state.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const LandedFrame f =
        land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 2.0, 24.0), -50.0);  // HA -2 h, east
    REQUIRE(f.side_of_pier == 1);
    REQUIRE(f.a2 < -1.0);

    driver->find_home();
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return driver->get_at_home(); }, std::chrono::seconds(60)));
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(5)));
    INFO("after FindHome a2=" << mount.physical_degrees(2) << " reported RA " << driver->get_right_ascension());
    CHECK(driver->get_side_of_pier() == 0);
    CHECK(std::abs(wrap_ha(driver->get_right_ascension() - (driver->get_sidereal_time() - 6.0))) < 0.05);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - FindHome on a k = -1 board reports the side its position implies (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // The #459 home cases above all run where k = +1. On the EQM-35 Pro north
    // of the equator k = -1, so the positive branch the home goto leaves in
    // memory reads HA -6 h and pierWest. What must hold on either sense is that
    // SideOfPier agrees with DestinationSideOfPier for the reported position.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 35.0, 11.0, 100.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    // Start on the other branch: west of the meridian is pierEast, a2 < 0 here.
    const double lst = driver->get_sidereal_time();
    const LandedFrame f =
        land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 22.0, 24.0), 50.0);  // HA +2 h, west
    REQUIRE(f.side_of_pier == 0);
    REQUIRE(f.a2 < -1.0);

    driver->find_home();
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return driver->get_at_home(); }, std::chrono::seconds(60)));
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(5)));
    const double ra = driver->get_right_ascension();
    const double dec = driver->get_declination();
    INFO("after FindHome a2=" << mount.physical_degrees(2) << " reported RA " << ra << " dec " << dec);
    REQUIRE(std::abs(dec - 90.0) < 0.2);
    CHECK(driver->get_side_of_pier() == driver->get_destination_side_of_pier(ra, dec));
    CHECK(driver->get_side_of_pier() == 1);
    CHECK(std::abs(wrap_ha(ra - (driver->get_sidereal_time() + 6.0))) < 0.05);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an unidentified board warns that it lost its measured sense (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // The dec-axis sense is looked up by the ":e" mount code, so a board whose
    // identify fails on connect runs the session on the unmeasured model: an
    // EQM-35 Pro north of the equator is then 12 h out. The connect must still
    // succeed (a board that will not identify is usable), but it must say so.
    std::atomic<int> warns{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Warn &&
                message.find("Motor board not identified") != std::string::npos) {
                ++warns;
            }
        });

    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    mount.set_garbled_version_replies(true);
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 35.0, 11.0, 100.0);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(warns.load() == 1);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a reconnect that fails to identify forgets the measured sense (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // The sense belongs to the board that answered ":e" on THIS connect. An
    // EQM-35 Pro north of the equator runs k = -1; when the same driver
    // reconnects and the identify fails, the session must fall back to the
    // unmeasured model (k = +1), not keep the previous connection's -1.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = 37.2;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 174.88, 80.0);
    driver->set_connected(true);
    driver->set_connected(false);
    mount.set_garbled_version_replies(true);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = 30.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1, 0);
    // The unmeasured branch; the stale -1 would have put a2 at +60.
    CHECK(f.a2 < 0.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an unidentified board points on the unmeasured model, south (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // With no mount code there is no measured sense, so k = +1 in both
    // hemispheres, the model every unmeasured board shipped with. South of the
    // equator that differs from k = s, the answer for a measured eps = +1, so
    // this is the hemisphere where a wrong fallback shows.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    mount.set_garbled_version_replies(true);
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = -30.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1, 0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - the default Park lands on the positive branch (#459)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // Default park is the home position, a goto to a2 = +0.0, so the parked
    // report agrees with FindHome and with connect.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 45.0, 11.0, 100.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 2.0, 24.0), 40.0);  // HA -2 h, east
    REQUIRE(f.side_of_pier == 1);

    driver->park();
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return driver->get_at_park(); }, std::chrono::seconds(60)));
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(5)));
    INFO("parked a2=" << mount.physical_degrees(2));
    CHECK(driver->get_side_of_pier() == 0);

    driver->unpark();
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a reconnect forgets the commanded branch (#459)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // reset_runtime_state_locked() puts the memory back on the positive
    // branch at connect. Observable only across a reconnect of the same
    // instance: park the memory negative with a pole goto, reconnect, and
    // the pole must report the positive side again.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 45.0, 11.0, 100.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const LandedFrame pole = land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 0.1, 24.0), 90.0);  // HA -0.1 h
    REQUIRE(pole.side_of_pier == 1);

    driver->set_tracking(false);
    driver->set_connected(false);
    driver->set_connected(true);
    INFO("after reconnect a2=" << mount.physical_degrees(2));
    CHECK(driver->get_side_of_pier() == 0);

    driver->set_connected(false);
}

namespace {

// open-astro#458: the Wave 100i capture with only the ":e" reply swapped for
// the Wave 150i's (fw 3.59, mount code 0x45, from the TRACE log attached to
// open-astro#230). Not a capture of the 150i: that board reports a different
// CPR per axis (:a1 3878400, :a2 3525120). The fake can model that
// (FakeMountProfile::cpr_dec, open-astro#579), but this profile doesn't set
// it, so it stays a Wave 100i geometry with the 150i's mount code. The mount
// code is the only field these cases depend on.
alpacacore::test::FakeMountProfile wave_150i_mount_code() {
    alpacacore::test::FakeMountProfile p = alpacacore::test::FakeMountProfile::wave_100i();
    p.version_reply = "033B45";
    return p;
}

}  // namespace

TEST_CASE("SkyWatcher pointing - the hemisphere-symmetric model agrees with indi-eqmod (#458)",
          "[skywatcher][telescope][pointing][hemisphere]") {
    // The eps = +1 case of sky_from_axes() and an independent transcription of
    // indi-eqmod agree everywhere a goto can reach, in both hemispheres. The
    // #432 model agrees with indi-eqmod in the north only, and is 12 h out in
    // the south: that difference IS #458.
    for (const double latitude : {45.0, -37.2}) {
        for (double a1 = -90.0; a1 <= 90.0; a1 += 15.0) {
            for (double a2 = -170.0; a2 <= 170.0; a2 += 17.0) {
                if (std::abs(a2) < 1e-9) {
                    // The pole: dec = 90 and the hour angle is undefined, so
                    // the two formulas may name different branches (#459).
                    continue;
                }
                const SkyPoint model = sky_from_axes(latitude, a1, a2, +1);
                const SkyPoint indi = sky_from_indi_eqmod(latitude, a1, a2);
                const SkyPoint old_model = sky_from_axes(latitude, a1, a2);
                INFO("lat " << latitude << " a1 " << a1 << " a2 " << a2 << ": model HA " << model.ha_hours << " dec "
                            << model.dec_degrees << ", indi HA " << indi.ha_hours << " dec " << indi.dec_degrees);
                CHECK(std::abs(wrap_ha(model.ha_hours - indi.ha_hours)) < 1e-9);
                CHECK(std::abs(model.dec_degrees - indi.dec_degrees) < 1e-9);
                const double old_gap = std::abs(wrap_ha(old_model.ha_hours - indi.ha_hours));
                CHECK(std::abs(old_gap - (latitude < 0.0 ? 12.0 : 0.0)) < 1e-9);
            }
        }
    }
}

TEST_CASE("SkyWatcher pointing - an EQM-35 Pro goto east of the meridian lands on the sky, north (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // The hardware row 5 goto, now judged by where the EQM-35 board physically
    // points. Before #458 the driver sent a1 = +45, a2 = -60 here, and the
    // saddle ended at HA +9 h, below the horizon.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = 37.2;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 174.88, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = 30.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1, -1);
    // The RA axis turns the same way as on a board that counts the other
    // way; only the dec axis swings to the other side of the meridian.
    CHECK(std::abs(f.a1 - 45.0) < 1.0);
    CHECK(std::abs(f.a2 - 60.0) < 0.2);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an EQM-35 Pro goto west of the meridian lands on the sky, north (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = 37.2;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 174.88, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);  // HA +3 h
    const double target_dec = 30.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0, -1);
    CHECK(std::abs(f.a1 + 45.0) < 1.0);
    CHECK(std::abs(f.a2 + 60.0) < 0.2);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - DeclinationRate raises the physical Dec of an EQM-35 Pro, north (#458)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // The Dec rate and guide signs read the branch and the hemisphere only:
    // dec = s * (90 - |a2|) does not involve the board's sense. Pinned on the
    // branch the #458 goto now lands on, judged by the physical Dec.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = 37.2;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 174.88, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, std::fmod(lst + 3.0, 24.0), 30.0);  // HA -3 h
    REQUIRE(f.a2 > 0.0);

    const double dec_start =
        sky_from_axes(latitude, mount.physical_degrees(1), mount.physical_degrees(2), -1).dec_degrees;
    driver->set_declination_rate(10.0);  // arcsec/s, well above the ~0.26 floor
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return mount.axis_running(2); }, std::chrono::seconds(3)));
    // Real-rate motion: no goto runs here, so virtual time follows wall time
    // for the 2 s the rate is measured over.
    const auto rate_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    run_until_on_fake_clock(
        clock, mount, [&] { return std::chrono::steady_clock::now() >= rate_end; }, std::chrono::seconds(3));
    const double dec_end =
        sky_from_axes(latitude, mount.physical_degrees(1), mount.physical_degrees(2), -1).dec_degrees;
    const double moved_arcsec = (dec_end - dec_start) * 3600.0;
    INFO("physical Dec moved " << moved_arcsec << " arcsec");
    CHECK(moved_arcsec > 10.0);
    CHECK(moved_arcsec < 40.0);

    driver->set_declination_rate(0.0);
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !mount.axis_running(2); }, std::chrono::seconds(5)));
    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a Wave 150i goto lands on the sky, south (#458, geometry only)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // NOT MEASURED. The Wave 150i's sense (+1) is measured in the north only
    // (hardware row 4); this case is what geometry says the same board does
    // south of the equator, where the #432 model would put it 12 h out.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(wave_150i_mount_code(), clock);
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = -30.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1, +1);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an unmeasured board keeps the #432 model in both hemispheres (#458)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // Mount code 0x44 (Wave 100i) has no measured sense, so the driver must
    // not guess one: gotos land where the #432 model says, north and south.
    for (const double latitude : {45.0, -35.0}) {
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
        driver->set_connected(true);
        driver->set_tracking(true);

        const double lst = driver->get_sidereal_time();
        const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
        const double target_dec = latitude < 0.0 ? -30.0 : 30.0;
        INFO("latitude " << latitude);
        const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
        check_landing(f, latitude, target_ra, target_dec, 1);

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

// A plate-solving client syncs on a target, slews away and slews back. The
// only other sync in this file is at the exact pole (#459), so nothing here
// pinned a sync anywhere else, or that a goto after it still lands on the
// sky. The return trip has to put the tube back where the sync was made;
// a1 is allowed to differ by the sidereal motion tracking adds while the test
// runs (15 deg an hour, so 1.5 deg is six minutes).
TEST_CASE("SkyWatcher pointing - a sync away from the pole survives a goto and the way back, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);

    const double lst = driver->get_sidereal_time();
    const double sync_ra = std::fmod(lst - 2.0 + 24.0, 24.0);  // HA +2 h
    const double sync_dec = -30.0;
    driver->sync_to_coordinates(sync_ra, sync_dec);

    // A sync is the controller's ":E" count re-stamp: no motor moves, so the
    // axes stay physically at home while the driver's frame jumps to the
    // synced target. That is why the landings below are judged by the
    // driver's own readback and by where the axes really are relative to the
    // sync, never through sky_from_axes()/check_landing(), which assume the
    // physical axes and the counts share one frame.
    CHECK(std::abs(mount.physical_degrees(1)) < 0.1);
    CHECK(std::abs(mount.physical_degrees(2)) < 0.1);
    CHECK(std::abs(wrap_ha(driver->get_right_ascension() - sync_ra)) < kHaToleranceHours);
    CHECK(std::abs(driver->get_declination() - sync_dec) < kDecToleranceDegrees);
    CHECK(driver->get_side_of_pier() == 0);

    driver->set_tracking(true);

    // Same side of the meridian, different HA and dec: no flip involved. The
    // tube has to move, and the readback has to follow the new target.
    const double other_ra = std::fmod(lst - 4.0 + 24.0, 24.0);  // HA +4 h
    const double other_dec = -55.0;
    const LandedFrame away = land_on_fake_clock(clock, *driver, mount, other_ra, other_dec);
    INFO("away: physical a1=" << away.a1 << " a2=" << away.a2);
    CHECK(std::abs(wrap_ha(away.reported_ra - other_ra)) < kHaToleranceHours);
    CHECK(std::abs(away.reported_dec - other_dec) < kDecToleranceDegrees);
    CHECK(away.side_of_pier == 0);
    CHECK(std::abs(away.a1) > 10.0);
    CHECK(std::abs(away.a2) > 10.0);

    // Back on the synced target the tube is where the sync was made, i.e.
    // home, apart from the tracking drift.
    const LandedFrame back = land_on_fake_clock(clock, *driver, mount, sync_ra, sync_dec);
    INFO("back: physical a1=" << back.a1 << " a2=" << back.a2);
    CHECK(std::abs(wrap_ha(back.reported_ra - sync_ra)) < kHaToleranceHours);
    CHECK(std::abs(back.reported_dec - sync_dec) < kDecToleranceDegrees);
    CHECK(back.side_of_pier == 0);
    CHECK(std::abs(back.a2) < 0.05);
    CHECK(std::abs(back.a1) < 1.5);

    driver->set_tracking(false);
    driver->set_connected(false);
}

// A west goto, an east goto (the flip), and a west goto again. The existing
// west and east cases each start from home; the other branch changes in this
// file all start or end at the pole (#459). None goes from one side of the
// meridian to the other and back. The return to the first target has to reproduce the
// first landing's axes: a2 exactly (it does not depend on time), a1 within the
// tracking drift while the test runs.
TEST_CASE("SkyWatcher pointing - consecutive meridian flips return to the same axes, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double west_ra = std::fmod(lst - 2.0 + 24.0, 24.0);  // HA +2 h
    const double west_dec = -30.0;
    const double east_ra = std::fmod(lst + 2.5, 24.0);  // HA -2.5 h
    const double east_dec = -50.0;
    REQUIRE(driver->get_destination_side_of_pier(west_ra, west_dec) == 0);
    REQUIRE(driver->get_destination_side_of_pier(east_ra, east_dec) == 1);

    const LandedFrame first = land_on_fake_clock(clock, *driver, mount, west_ra, west_dec);
    check_landing(first, latitude, west_ra, west_dec, 0, -1);
    CHECK(first.a2 > 0.0);

    const LandedFrame flipped = land_on_fake_clock(clock, *driver, mount, east_ra, east_dec);
    check_landing(flipped, latitude, east_ra, east_dec, 1, -1);
    CHECK(flipped.a2 < 0.0);

    const LandedFrame back = land_on_fake_clock(clock, *driver, mount, west_ra, west_dec);
    check_landing(back, latitude, west_ra, west_dec, 0, -1);
    CHECK(back.a2 > 0.0);
    CHECK(std::abs(back.a2 - first.a2) < 0.05);
    CHECK(std::abs(back.a1 - first.a1) < 1.5);

    driver->set_tracking(false);
    driver->set_connected(false);
}

// The side and the dec branch are chosen from the sky hour angle, so they
// change at HA 0 and nowhere else. The pole cases already pin that at HA
// +/-0.1 h, but only at dec 90; every other goto away from the pole sits 2 h or
// more from the meridian. This case pins the switch away from the pole, 0.05 h
// either side, with the RA axis at the counterweight limit. The
// targets sit 0.05 h either side because LST cannot be frozen: a target at
// exactly HA 0 would land on either side depending on when the driver reads
// the clock. The driver aims ahead by an estimated slew time (distance over
// the max rate plus goto overhead and resume latency) before it picks the
// branch, so the test's LST read and the driver's own evaluation must stay
// within 3 min (0.05 h) of each other; the simulated slew is about 27 s, so a
// wrong `expected_side` here is a timing budget to look at before it is a
// driver regression.
// At HA +/-0.05 h the RA axis is within 0.75 deg of +/-90, the
// counterweight-horizontal limit, and the two landings are on opposite dec
// branches.
TEST_CASE("SkyWatcher pointing - the pier side changes at HA 0 and the axes stay inside the limit, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    const double latitude = -35.0;
    for (const double ha : {+0.05, -0.05}) {
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro(), clock);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
        driver->set_connected(true);
        driver->set_tracking(true);

        const double lst = driver->get_sidereal_time();
        const double target_ra = std::fmod(lst - ha + 24.0, 24.0);
        const double target_dec = -30.0;
        const int side = ha >= 0.0 ? 0 : 1;
        INFO("target HA " << ha << " h");
        REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == side);

        const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, target_dec);
        check_landing(f, latitude, target_ra, target_dec, side, -1);
        if (ha > 0.0) {
            CHECK(f.a2 > 0.0);
            CHECK(f.a1 > 85.0);
        } else {
            CHECK(f.a2 < 0.0);
            CHECK(f.a1 < -85.0);
        }

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

// Hardware rows measured against the sky. EQM-35 Pro (0x32) at latitude -37.1
// (rounded), 2026-09-24. Each row pairs the physical axis
// angles at a 5 s exposure with where ASTAP plate-solved that exposure, precessed
// to the equinox of date. The axes are the board's own :j replies from the
// driver's TRACE log, read within 2.7 s of the exposure midpoint, with both :E
// sync re-stamps taken out, so they are where the axes really were, not what
// the driver was told. Two power-ons; the mount was hand-homed before each.
//
// The tolerances are set by the mount, not by the model. A seven-term fit to
// 15 solved exposures (these 14 rows and the home row) puts the polar axis
// 1.3 deg off the pole, the tube 0.8 deg off
// square to the dec axis, the dec zero 0.8 and 3.3 deg off and the RA zero 3.3
// and 6.2 deg off (first and second power-on), with 8 arcmin rms left over. The
// plain model below has none of those terms, so it is up to 0.43 h and 4.6 deg
// from the sky. The checks are sized to catch what this file exists to catch:
// a 6 h or 12 h hour-angle error, a wrong sign, a wrong dec branch across the
// meridian, or a wrong RA-axis scale or direction. The last is the within-
// power-on check: the RA zero cancels there, so rows on one side of the
// meridian must share one HA offset while a1 spans 38.7 to 80.4 deg (first
// power-on, west) and 19.0 to 72.1 deg (second power-on, west).
// The home row is left out: at a2 = 0 the dec branch is undefined (#459).
// Before adding a row, check its residuals against the bounds below: the dec
// bound has only 0.42 deg of margin here (4.58 measured) and the HA bound
// 0.066 h (0.434 measured, "Dec -40 circle 1"), and both margins are set by
// this rig's own zero errors, so a row from a worse-homed rig would fail on the
// rig, not on the model. The same-side spread only constrains groups with two
// or more rows: power-on 2 east has one, so its check compares it to itself.
TEST_CASE("SkyWatcher pointing - measured axes agree with the plate-solved sky across a flip, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    struct Row {
        const char* what;
        int power_on;
        double a1;
        double a2;
        double solved_ha;   // hours, of date
        double solved_dec;  // degrees, of date
    };
    const Row rows[] = {
        {"M7 first", 1, 59.130, 55.210, 2.291, -34.630},
        {"M7 at the sync", 1, 58.508, 55.210, 2.331, -34.639},
        {"NGC 6752, west", 1, 80.393, 29.869, 0.897, -59.786},
        {"Antares field, west", 1, 38.678, 63.349, 3.664, -26.796},
        {"IC 5148 after a flip, east", 1, -60.436, -50.801, -1.791, -37.117},
        {"Capricornus field, east", 1, -72.167, -54.151, -1.017, -33.710},
        {"Dec -40 circle 1, west", 2, 72.073, 50.000, 1.629, -42.252},
        {"Dec -40 circle 2, west", 2, 53.910, 50.000, 2.817, -42.549},
        {"Dec -40 circle 3, west", 2, 35.750, 50.000, 4.024, -42.874},
        {"Dec -40 circle 4, west", 2, 19.579, 50.000, 5.105, -43.175},
        {"at the second sync, west", 2, 18.963, 50.000, 5.145, -43.186},
        {"M7, west", 2, 54.214, 58.396, 2.812, -34.121},
        {"IC 5148 after a flip, east", 2, -65.344, -47.464, -1.297, -37.956},
        {"M7 after the flip back, west", 2, 53.241, 58.396, 2.877, -34.143},
    };
    constexpr double latitude = -37.1;
    constexpr double kRowHaToleranceHours = 0.5;     // 7.5 deg; a 6 h error is twelve times this
    constexpr double kRowDecToleranceDegrees = 5.0;  // a wrong hemisphere sign is 53 deg or more here
    constexpr double kSameSideSpreadHours = 0.05;    // 3 min; measured spread is under 2 min

    // First HA offset (model - sky) seen per power-on and side, for the spread check.
    double first_offset[3][2] = {};
    bool seen[3][2] = {};
    for (const Row& r : rows) {
        REQUIRE((r.power_on == 1 || r.power_on == 2));  // indexes first_offset / seen below
        const SkyPoint sky = sky_from_axes(latitude, r.a1, r.a2, -1);
        const double ha_offset = wrap_ha(sky.ha_hours - r.solved_ha);
        INFO(r.what << " (power-on " << r.power_on << "): model HA " << sky.ha_hours << " h dec " << sky.dec_degrees
                    << "; solved HA " << r.solved_ha << " h dec " << r.solved_dec);
        CHECK(std::abs(ha_offset) < kRowHaToleranceHours);
        CHECK(std::abs(sky.dec_degrees - r.solved_dec) < kRowDecToleranceDegrees);
        // Data check on the recorded rows: each landing's dec branch is the side of
        // the meridian the sky put the tube on. A wrong branch in the model is caught
        // by the HA check above, not by this line. This assumes this board's
        // k = s * eps = +1 (EQM-35 Pro, south): an eps = +1 board in the south
        // would legitimately fail it.
        CHECK((r.solved_ha >= 0.0) == (r.a2 >= 0.0));

        const int side = r.a2 >= 0.0 ? 0 : 1;
        if (!seen[r.power_on][side]) {
            seen[r.power_on][side] = true;
            first_offset[r.power_on][side] = ha_offset;
        }
        CHECK(std::abs(wrap_ha(ha_offset - first_offset[r.power_on][side])) < kSameSideSpreadHours);
    }
}

// open-astro#715: the driver decided a goto had landed by waiting out a timer
// (slew_force_until_: 8 s at dispatch, 3 s per refinement) instead of asking
// the board, so no goto could report done in under 8 s however short the
// move, and that floor trained goto_overhead_seconds_ (one of #580's two
// causes). On the fake clock the board lands the instant the test says so,
// which makes the timer the only thing left between "stopped" and "landed".
TEST_CASE("SkyWatcher pointing - landing is reported when the board reports stopped, not at the 8 s floor (#715)",
          "[skywatcher][telescope][pointing][taskclock]") {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    const double latitude = 45.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 100.0);
    driver->set_connected(true);
    // Tracking stays off: the RA axis is stationary between gotos, so no
    // sidereal motion in virtual time can drift from the driver's wall-clock
    // LST, and the landing is the only thing being timed.
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 1.0 + 24.0, 24.0);  // HA +1 h
    const double target_dec = 30.0;                              // 60 deg on the dec axis: 18 s at 800x sidereal
    const int expected_side = driver->get_destination_side_of_pier(target_ra, target_dec);

    const int starts_before = mount.frames_seen('J');
    driver->slew_to_coordinates_async(target_ra, target_dec);
    REQUIRE(driver->get_slewing());
    // Both axes started: the board reports RUNNING in goto mode from here.
    REQUIRE(mount.wait_for_frames('J', starts_before + 2, std::chrono::seconds(5)));
    const auto t_started = std::chrono::steady_clock::now();
    const int polls_at_start = mount.frames_seen('f');

    // 1. The driver asks the board whether it is still moving: a ":f" reaches
    //    the board within one poll period (250 ms) of the start, not after an
    //    8 s window during which the board is never asked.
    CHECK(mount.wait_for_frames('f', polls_at_start + 1, std::chrono::seconds(2)));

    // 2. Land every goto (the first and each refinement) the instant the
    //    board starts it. Slewing must then clear a few polls later: well
    //    under the 8 s floor, and under the 8 + 3 + 3 s a refined goto had.
    REQUIRE(run_until_on_fake_clock(clock, mount, [&] { return !driver->get_slewing(); }, std::chrono::seconds(30)));
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_started).count();
    const int polls = mount.frames_seen('f') - polls_at_start;
    INFO("Slewing cleared " << took << " s after the goto started; " << polls << " status polls reached the board");
    CHECK(!mount.axis_running(1));
    CHECK(!mount.axis_running(2));
    CHECK(polls > 0);
    CHECK(took < 5.0);

    // 3. And it landed where it was sent: no tolerance loosened for the clock.
    LandedFrame f{};
    f.lst = driver->get_sidereal_time();
    f.a1 = mount.physical_degrees(1);
    f.a2 = mount.physical_degrees(2);
    f.reported_ra = driver->get_right_ascension();
    f.reported_dec = driver->get_declination();
    f.side_of_pier = driver->get_side_of_pier();
    check_landing(f, latitude, target_ra, target_dec, expected_side);

    driver->set_connected(false);
}

// ── decAxisSense override (open-astro#582) ──────────────────────────────────
namespace {

alpacacore::test::FakeMountProfile wave_150i_profile() {
    auto p = alpacacore::test::FakeMountProfile::wave_100i();
    p.version_reply = "033B45";  // fw 3.59, mount code 0x45
    return p;
}

// Connects over `profile` with the given setting, goes to HA -3 h at `dec`,
// checks the landing against eps `expected_eps` and that SideOfPier agrees
// with DestinationSideOfPier, and that a2 has the sign eps implies.
void connect_and_land(const alpacacore::test::FakeMountProfile& profile, sw::DecAxisSenseSetting setting,
                      double latitude, double dec, int expected_eps, int expected_a2_sign, bool garble = false) {
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(profile, clock);
    REQUIRE(mount.ok());
    if (garble) {
        mount.set_garbled_version_replies(true);
    }
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0, {}, {}, clock, setting);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    // HA -3 h is the east side whatever eps is: the side follows the sky hour
    // angle, and eps moves the dec-axis angle (the sign of a2) instead.
    const int destination_side = driver->get_destination_side_of_pier(target_ra, dec);
    CHECK(destination_side == 1);

    const LandedFrame f = land_on_fake_clock(clock, *driver, mount, target_ra, dec);
    check_landing(f, latitude, target_ra, dec, 1, expected_eps);
    CHECK((f.a2 > 0.0 ? 1 : -1) == expected_a2_sign);
    CHECK(f.side_of_pier == destination_side);

    driver->set_tracking(false);
    driver->set_connected(false);
}

}  // namespace

TEST_CASE("SkyWatcher pointing - decAxisSense normal beats the measured -1 of an EQM-35 Pro (#582)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    // eps = +1 north is the unmeasured model's k = +1: a2 < 0 at HA -3 h.
    connect_and_land(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Normal, 37.2, 30.0, +1,
                     -1);
}

TEST_CASE("SkyWatcher pointing - decAxisSense reversed beats the measured +1 of a Wave 150i (#582)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    connect_and_land(wave_150i_profile(), sw::DecAxisSenseSetting::Reversed, 37.2, 30.0, -1, +1);
}

TEST_CASE("SkyWatcher pointing - decAxisSense auto keeps the measured table (#582)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere][taskclock]") {
    connect_and_land(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Auto, 37.2, 30.0, -1,
                     +1);
    connect_and_land(wave_150i_profile(), sw::DecAxisSenseSetting::Auto, 37.2, 30.0, +1, -1);
}

TEST_CASE("SkyWatcher pointing - decAxisSense applies when the board will not identify itself (#582)",
          "[skywatcher][telescope][pointing][hemisphere][taskclock]") {
    // North: the unmeasured default is k = +1 (a2 < 0); reversed is k = -1 (a2 > 0).
    connect_and_land(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Reversed, 37.2, 30.0, -1,
                     +1, true);
    // South: normal is k = s = -1, which puts a2 on the other branch (a2 > 0).
    connect_and_land(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Normal, -35.0, -30.0, +1,
                     +1, true);
}

TEST_CASE("SkyWatcher pointing - the connect log names where eps came from (#582)",
          "[skywatcher][telescope][pointing][taskclock]") {
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    std::mutex log_mutex;
    std::vector<std::pair<alpacacore::logging::LogLevel, std::string>> lines;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
            std::lock_guard<std::mutex> lock(log_mutex);
            lines.emplace_back(level, std::string(message));
        });
    using alpacacore::logging::LogLevel;
    const auto connect_log = [&](const alpacacore::test::FakeMountProfile& profile, sw::DecAxisSenseSetting setting,
                                 bool garble) {
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            lines.clear();
        }
        alpacacore::test::FakeTaskClock clock;
        FakeSkyWatcherMount mount(profile, clock);
        REQUIRE(mount.ok());
        if (garble) {
            mount.set_garbled_version_replies(true);
        }
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), 35.0, 11.0, 100.0, {}, {}, clock, setting);
        driver->set_connected(true);
        const std::string name = driver->get_name();
        driver->set_connected(false);
        std::lock_guard<std::mutex> lock(log_mutex);
        return std::make_pair(name, lines);
    };
    const auto find_line = [](const auto& captured, const std::string& needle) -> const LogLevel* {
        for (const auto& [level, text] : captured) {
            if (text.find(needle) != std::string::npos) return &level;
        }
        return nullptr;
    };

    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Auto, false);
        CHECK(find_line(captured, "from the measured table"));
        CHECK(name == "Sky-Watcher EQM-35 Pro (EQMOD)");
    }
    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::wave_100i(), sw::DecAxisSenseSetting::Auto, false);
        CHECK(find_line(captured, "unmeasured-board default"));
    }
    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::wave_100i(), sw::DecAxisSenseSetting::Reversed, false);
        const LogLevel* level = find_line(captured, "from the user override");
        REQUIRE(level);
        CHECK(*level == LogLevel::Info);
        CHECK(name == "Sky-Watcher Wave 100i (EQMOD, dec axis sense: reversed)");
    }
    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Normal, false);
        const LogLevel* level = find_line(captured, "disagrees with the measured value");
        REQUIRE(level);
        CHECK(*level == LogLevel::Warn);
    }
    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Auto, true);
        CHECK(find_line(captured, "Motor board not identified"));
        CHECK(find_line(captured, "unmeasured-board default"));
    }
    {
        auto [name, captured] =
            connect_log(alpacacore::test::FakeMountProfile::eqm35_pro(), sw::DecAxisSenseSetting::Reversed, true);
        CHECK(find_line(captured, "Motor board not identified"));
        CHECK(find_line(captured, "from the user override"));
    }
}

#endif  // !_WIN32
