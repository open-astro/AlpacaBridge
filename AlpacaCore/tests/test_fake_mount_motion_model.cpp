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

// FakeMountMotionModel (#2614): the protocol-neutral mount motion the fake ConformU rows share. Each case drives
// the model on a hand-advanced clock and reads what a protocol fake would answer.

#include <chrono>
#include <cmath>
#include <memory>

#include "catch2_compat.h"
#include "fake_mount_motion_model.h"

namespace {

using alpacacore::test::FakeMountMotionModel;
using Catch::Approx;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Axis = FakeMountMotionModel::Axis;

// A fixed UTC instant (2026-01-01 00:00:00) the test moves by hand.
struct Rig {
    std::shared_ptr<FakeMountMotionModel::TimePoint> now = std::make_shared<FakeMountMotionModel::TimePoint>(
        FakeMountMotionModel::TimePoint{std::chrono::seconds(1767225600)});
    FakeMountMotionModel model = FakeMountMotionModel([this] {
        FakeMountMotionModel::Config config;
        config.clock = [t = now] { return *t; };
        return config;
    }());
    void advance(std::chrono::milliseconds by) { *now += by; }
};

constexpr double kSidereal = FakeMountMotionModel::kSiderealDegPerSec;

}  // namespace

TEST_CASE("FakeMountMotionModel: a GOTO reaches its target only after the travel time", "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(0.0, -90.0);
    rig.model.set_tracking(true);
    const auto travel = rig.model.goto_radec(2.0, -30.0);  // 60 degrees of Dec at 6 deg/s
    CHECK(travel.count() == Approx(10.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.slewing());

    rig.advance(seconds(5));
    CHECK(rig.model.slewing());
    CHECK(rig.model.dec_degrees() == Approx(-60.0).epsilon(0).margin(0.01));  // half way
    CHECK(rig.model.dec_degrees() != Approx(-30.0).epsilon(0).margin(1.0));

    rig.advance(seconds(5));
    CHECK_FALSE(rig.model.slewing());
    CHECK(rig.model.ra_hours() == Approx(2.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.dec_degrees() == Approx(-30.0).epsilon(0).margin(1e-9));
}

TEST_CASE("FakeMountMotionModel: tracking holds the sky position and tracking off drifts it east",
          "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(5.0, -20.0);
    rig.model.set_tracking(true);
    rig.advance(seconds(3600));
    CHECK(rig.model.ra_hours() == Approx(5.0).epsilon(0).margin(1e-9));

    rig.model.set_tracking(false);
    rig.advance(seconds(3600));
    // One sidereal hour of drift: kSidereal degrees per second.
    CHECK(rig.model.ra_hours() == Approx(5.0 + kSidereal * 3600.0 / 15.0).epsilon(0).margin(1e-6));
    CHECK(rig.model.ra_hours() > 5.9);
}

TEST_CASE("FakeMountMotionModel: a pulse moves the axis for its duration and then stops", "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(5.0, -20.0);
    rig.model.set_tracking(true);
    rig.model.pulse(Axis::Dec, 0.5, milliseconds(2000));
    CHECK(rig.model.pulse_active());

    rig.advance(milliseconds(1000));
    CHECK(rig.model.dec_degrees() == Approx(-19.5).epsilon(0).margin(1e-9));
    CHECK(rig.model.pulse_active());

    rig.advance(milliseconds(5000));  // well past the pulse: it moved 2 s, not 6 s
    CHECK_FALSE(rig.model.pulse_active());
    CHECK(rig.model.dec_degrees() == Approx(-19.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.ra_hours() == Approx(5.0).epsilon(0).margin(1e-9));
}

TEST_CASE("FakeMountMotionModel: MoveAxis runs each axis at its signed rate until stopped",
          "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(5.0, 0.0);
    rig.model.set_tracking(true);
    rig.model.move_axis(Axis::Ra, 1.5);    // RA rises
    rig.model.move_axis(Axis::Dec, -2.0);  // Dec falls
    CHECK(rig.model.axis_moving(Axis::Ra));
    CHECK(rig.model.axis_moving(Axis::Dec));

    rig.advance(seconds(10));
    CHECK(rig.model.ra_hours() == Approx(5.0 + 15.0 / 15.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.dec_degrees() == Approx(-20.0).epsilon(0).margin(1e-9));

    rig.model.move_axis(Axis::Ra, 0.0);
    rig.model.move_axis(Axis::Dec, 0.0);
    CHECK_FALSE(rig.model.axis_moving(Axis::Ra));
    rig.advance(seconds(10));
    CHECK(rig.model.ra_hours() == Approx(6.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.dec_degrees() == Approx(-20.0).epsilon(0).margin(1e-9));
}

TEST_CASE("FakeMountMotionModel: stop freezes a slew where it is", "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(0.0, -90.0);
    rig.model.set_tracking(true);
    (void)rig.model.goto_radec(0.0, 0.0);  // 90 degrees, 15 s
    rig.advance(seconds(5));
    rig.model.stop();
    CHECK_FALSE(rig.model.slewing());
    const double stopped_at = rig.model.dec_degrees();
    CHECK(stopped_at == Approx(-60.0).epsilon(0).margin(0.01));
    rig.advance(seconds(60));
    CHECK(rig.model.dec_degrees() == Approx(stopped_at).epsilon(0).margin(1e-9));
}

TEST_CASE("FakeMountMotionModel: park reports parked on arrival and home reports at-home",
          "[fake][mount-model][unit]") {
    Rig rig;
    rig.model.set_position(3.0, 10.0);
    rig.model.set_tracking(true);
    rig.model.park();
    CHECK(rig.model.slewing());
    CHECK_FALSE(rig.model.parked());

    rig.advance(seconds(60));
    CHECK_FALSE(rig.model.slewing());
    CHECK(rig.model.parked());
    CHECK_FALSE(rig.model.tracking());
    CHECK(rig.model.dec_degrees() == Approx(-90.0).epsilon(0).margin(1e-9));
    CHECK(rig.model.hour_angle_hours() == Approx(0.0).epsilon(0).margin(1e-4));

    rig.advance(seconds(3600));  // parked: the axes hold, the sky does not
    CHECK(rig.model.hour_angle_hours() == Approx(0.0).epsilon(0).margin(1e-4));

    rig.model.unpark();
    CHECK_FALSE(rig.model.parked());

    rig.model.set_position(3.0, 10.0);
    rig.model.go_home();
    CHECK_FALSE(rig.model.at_home());
    rig.advance(seconds(60));
    CHECK(rig.model.at_home());
    CHECK_FALSE(rig.model.parked());
    rig.model.set_tracking(true);
    CHECK_FALSE(rig.model.at_home());
}

TEST_CASE("FakeMountMotionModel: the side of pier follows the hour angle", "[fake][mount-model][unit]") {
    Rig rig;
    const double lst = rig.model.lst_hours();
    rig.model.set_position(lst - 2.0, -30.0);  // hour angle +2 h: west of the meridian
    CHECK(rig.model.hour_angle_hours() == Approx(2.0).epsilon(0).margin(1e-6));
    CHECK(rig.model.pier_side() == 0);

    rig.model.set_position(lst + 2.0, -30.0);  // hour angle -2 h: east of the meridian
    CHECK(rig.model.hour_angle_hours() == Approx(-2.0).epsilon(0).margin(1e-6));
    CHECK(rig.model.pier_side() == 1);
}

TEST_CASE("FakeMountMotionModel: altitude and azimuth come from the hour angle and the site",
          "[fake][mount-model][unit]") {
    Rig rig;
    const double lst = rig.model.lst_hours();
    // On the meridian at the site's own latitude the altitude is 90.
    rig.model.set_position(lst, -33.856784);
    CHECK(rig.model.altitude_degrees() == Approx(90.0).epsilon(0).margin(1e-6));
    // On the meridian, 10 degrees north of the zenith's Dec: altitude 80, azimuth 0 (north).
    rig.model.set_position(lst, -23.856784);
    CHECK(rig.model.altitude_degrees() == Approx(80.0).epsilon(0).margin(1e-6));
    CHECK(rig.model.azimuth_degrees() == Approx(0.0).epsilon(0).margin(1e-6));
}
