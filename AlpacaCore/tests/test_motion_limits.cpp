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

// Unit cases for the pure MotionLimits type (open-astro#436). No driver, no
// clock, no I/O: the type answers "is this target allowed?" and "did this
// motion cross a limit?" from numbers alone, and these cases pin both
// answers, the operator-facing refusal text and the off-by-default rule.

#include <alpacacore/util/motion_limits.h>

#include <optional>
#include <string>

#include "catch2_compat.h"

using alpacacore::util::check_target;
using alpacacore::util::crossed;
using alpacacore::util::LimitCrossing;
using alpacacore::util::MotionLimits;
using alpacacore::util::MotionSample;

namespace {

MotionLimits floor_only(double deg) {
    MotionLimits limits;
    limits.min_altitude_deg = deg;
    return limits;
}

MotionLimits meridian_only(double minutes) {
    MotionLimits limits;
    limits.meridian_limit_minutes = minutes;
    return limits;
}

MotionSample sample(double altitude_deg, double azimuth_deg = 180.0, double counterweight_up_deg = -90.0) {
    MotionSample s;
    s.altitude_deg = altitude_deg;
    s.azimuth_deg = azimuth_deg;
    s.counterweight_up_deg = counterweight_up_deg;
    return s;
}

}  // namespace

TEST_CASE("MotionLimits - default-constructed limits are off and allow every target", "[util][limits][unit]") {
    const MotionLimits limits;
    CHECK_FALSE(limits.enabled());
    CHECK_FALSE(limits.altitude_limit_enabled());
    CHECK_FALSE(limits.meridian_limit_enabled());
    CHECK_FALSE(limits.min_altitude(0.0).has_value());
    // Even a target under the ground is allowed: off means off, so ConformU's
    // arbitrary targets and every existing install see no refusal.
    CHECK_FALSE(check_target(limits, -90.0, 0.0).has_value());
    CHECK_FALSE(check_target(limits, -0.1, 270.0).has_value());
    CHECK(crossed(limits, sample(20.0), sample(-10.0)) == LimitCrossing::None);
}

TEST_CASE("MotionLimits - min_altitude is the configured constant at every azimuth", "[util][limits][unit]") {
    const MotionLimits limits = floor_only(15.0);
    CHECK(limits.enabled());
    CHECK(limits.altitude_limit_enabled());
    CHECK_FALSE(limits.meridian_limit_enabled());
    // The azimuth argument is reserved for a horizon profile; today the floor
    // is flat, so every azimuth answers the same number.
    for (double az : {0.0, 90.0, 180.0, 270.0, 359.9}) {
        REQUIRE(limits.min_altitude(az).has_value());
        CHECK(*limits.min_altitude(az) == 15.0);
    }
}

TEST_CASE("MotionLimits - check_target refuses a target below the floor and names the setting",
          "[util][limits][unit]") {
    const MotionLimits limits = floor_only(15.0);
    const auto refusal = check_target(limits, 12.3, 120.0);
    REQUIRE(refusal.has_value());
    // The operator reads this in NINA, standing at the mount: it names the
    // setting to change, not internal state (#358 message rule).
    CHECK(*refusal == "target altitude 12.3 deg is below the Minimum altitude limit (15 deg) set in the device settings");
}

TEST_CASE("MotionLimits - check_target formats the altitude to one decimal and drops a trailing .0 on the limit",
          "[util][limits][unit]") {
    const auto rounded = check_target(floor_only(15.0), 12.34, 0.0);
    REQUIRE(rounded.has_value());
    CHECK(*rounded == "target altitude 12.3 deg is below the Minimum altitude limit (15 deg) set in the device settings");

    const auto fractional_limit = check_target(floor_only(12.5), 3.0, 0.0);
    REQUIRE(fractional_limit.has_value());
    CHECK(*fractional_limit ==
          "target altitude 3.0 deg is below the Minimum altitude limit (12.5 deg) set in the device settings");

    const auto negative = check_target(floor_only(0.0), -5.25, 0.0);
    REQUIRE(negative.has_value());
    CHECK(*negative == "target altitude -5.3 deg is below the Minimum altitude limit (0 deg) set in the device settings");
}

TEST_CASE("MotionLimits - check_target allows a target at or above the floor", "[util][limits][unit]") {
    const MotionLimits limits = floor_only(15.0);
    CHECK_FALSE(check_target(limits, 15.0, 0.0).has_value());  // at the floor is not below it
    CHECK_FALSE(check_target(limits, 15.01, 0.0).has_value());
    CHECK_FALSE(check_target(limits, 89.9, 0.0).has_value());
    // Just under is refused: the boundary is exact, not padded.
    CHECK(check_target(limits, 14.99, 0.0).has_value());
}

TEST_CASE("MotionLimits - check_target ignores the meridian limit", "[util][limits][unit]") {
    // #432 keeps every goto target on the counterweight-down side, so a goto
    // never needs the meridian check; the type must not invent one. A
    // meridian-only configuration therefore allows any altitude, even under
    // the horizon.
    const MotionLimits limits = meridian_only(15.0);
    CHECK(limits.enabled());
    CHECK_FALSE(limits.altitude_limit_enabled());
    CHECK_FALSE(limits.min_altitude(0.0).has_value());
    CHECK_FALSE(check_target(limits, -20.0, 0.0).has_value());
    CHECK_FALSE(check_target(limits, 5.0, 90.0).has_value());
}

TEST_CASE("MotionLimits - crossed reports an altitude-floor crossing only from inside to outside",
          "[util][limits][unit]") {
    const MotionLimits limits = floor_only(15.0);
    // Inside -> outside: the live guard must stop this.
    CHECK(crossed(limits, sample(15.0), sample(14.9)) == LimitCrossing::AltitudeFloor);
    CHECK(crossed(limits, sample(40.0), sample(10.0)) == LimitCrossing::AltitudeFloor);
    // Landing exactly on the floor is still inside.
    CHECK(crossed(limits, sample(20.0), sample(15.0)) == LimitCrossing::None);
    // Motion that STARTS outside is never stopped: a mount below the floor
    // must always be drivable back up, and a step further down is the
    // operator's call.
    CHECK(crossed(limits, sample(10.0), sample(5.0)) == LimitCrossing::None);
    CHECK(crossed(limits, sample(10.0), sample(20.0)) == LimitCrossing::None);
    // Staying inside is nothing.
    CHECK(crossed(limits, sample(30.0), sample(25.0)) == LimitCrossing::None);
}

TEST_CASE("MotionLimits - crossed reports a meridian crossing in minutes of hour angle", "[util][limits][unit]") {
    // 15 minutes of hour angle is 3.75 deg of RA-axis travel past horizontal
    // (4 min per deg), so the limit sits at counterweight_up_deg = 3.75.
    const MotionLimits limits = meridian_only(15.0);
    CHECK(crossed(limits, sample(40.0, 180.0, 3.0), sample(40.0, 180.0, 4.0)) == LimitCrossing::Meridian);
    CHECK(crossed(limits, sample(40.0, 180.0, -10.0), sample(40.0, 180.0, 3.76)) == LimitCrossing::Meridian);
    // Landing exactly on the limit is still inside.
    CHECK(crossed(limits, sample(40.0, 180.0, 3.0), sample(40.0, 180.0, 3.75)) == LimitCrossing::None);
    // Starting past the limit is not a crossing.
    CHECK(crossed(limits, sample(40.0, 180.0, 4.0), sample(40.0, 180.0, 5.0)) == LimitCrossing::None);
    // Coming back is not a crossing either.
    CHECK(crossed(limits, sample(40.0, 180.0, 5.0), sample(40.0, 180.0, 1.0)) == LimitCrossing::None);
    // Counterweight-down travel (negative) never crosses anything.
    CHECK(crossed(limits, sample(40.0, 180.0, -80.0), sample(40.0, 180.0, -20.0)) == LimitCrossing::None);
}

TEST_CASE("MotionLimits - crossed with only the other limit set stays quiet", "[util][limits][unit]") {
    // Meridian limit off: a counterweight rising past horizontal is not reported.
    CHECK(crossed(floor_only(15.0), sample(40.0, 180.0, 0.0), sample(40.0, 180.0, 30.0)) == LimitCrossing::None);
    // Altitude floor off: a dive under the horizon is not reported.
    CHECK(crossed(meridian_only(15.0), sample(40.0), sample(-5.0)) == LimitCrossing::None);
}

TEST_CASE("MotionLimits - crossed reports the altitude floor first when both cross at once", "[util][limits][unit]") {
    MotionLimits both;
    both.min_altitude_deg = 15.0;
    both.meridian_limit_minutes = 15.0;
    CHECK(crossed(both, sample(20.0, 180.0, 0.0), sample(10.0, 180.0, 10.0)) == LimitCrossing::AltitudeFloor);
    CHECK(crossed(both, sample(20.0, 180.0, 0.0), sample(20.0, 180.0, 10.0)) == LimitCrossing::Meridian);
    CHECK(crossed(both, sample(20.0, 180.0, 0.0), sample(10.0, 180.0, 0.0)) == LimitCrossing::AltitudeFloor);
}
