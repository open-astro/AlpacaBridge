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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/pointing.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using Catch::Approx;
namespace pointing = alpacacore::util;

namespace {

std::chrono::system_clock::time_point at_unix(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{} + std::chrono::seconds(seconds);
}

// Throws InvalidValue with exactly `message`, or the check fails.
template <typename Fn>
void require_invalid_value(Fn&& fn, const std::string& message) {
    try {
        fn();
        FAIL("no exception thrown, expected: " << message);
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::InvalidValue);
        CHECK(std::string(e.what()) == message);
    }
}

}  // namespace

// Published GMST (Meeus, Astronomical Algorithms ch. 12, and the USNO
// almanac): J2000.0 is 280.46061837 deg = 18.6973745 h, 2000-01-01 0h UT is
// 6h39m52.27s, 1987-04-10 0h UT is 13h10m46.3668s, and 1987-04-10 19:21:00 UT
// is 8h34m57.0896s.
// Falsified by: gmst_degrees() changing the 280.46061837 constant or rounding
// the 360.98564736629 rate (the 1987 rows drift by seconds).
TEST_CASE("pointing - gmst_degrees matches published GMST values", "[util][pointing]") {
    CHECK(pointing::gmst_degrees(at_unix(946728000)) == Approx(280.46061837).epsilon(0).margin(1e-6));
    CHECK(pointing::gmst_degrees(at_unix(946684800)) / 15.0 ==
          Approx(6.0 + 39.0 / 60.0 + 52.27 / 3600.0).epsilon(0).margin(2e-5));
    CHECK(pointing::gmst_degrees(at_unix(545011200)) / 15.0 ==
          Approx(13.0 + 10.0 / 60.0 + 46.3668 / 3600.0).epsilon(0).margin(2e-5));
    CHECK(pointing::gmst_degrees(at_unix(545080860)) / 15.0 ==
          Approx(8.0 + 34.0 / 60.0 + 57.0896 / 3600.0).epsilon(0).margin(2e-5));
}

// The 0h-UT form of the same polynomial (Meeus 12.3, seconds of sidereal time),
// evaluated at 1900-01-01 and 2100-01-01 0h UT where the t*t term is about
// 0.09 s.
// Falsified by: gmst_degrees() dropping the 0.000387933 * t * t term (0.093 s
// at |T| = 1). The t*t*t term is below 1e-6 s here and pinned by no case.
TEST_CASE("pointing - gmst_degrees agrees with the 0h-UT polynomial a century either side of J2000",
          "[util][pointing]") {
    struct Row {
        std::int64_t unix_seconds;
        double julian_date;
    };
    for (const Row row : {Row{-2208988800, 2415020.5}, Row{4102444800, 2488069.5}}) {
        const double t = (row.julian_date - 2451545.0) / 36525.0;
        double seconds = 24110.54841 + 8640184.812866 * t + 0.093104 * t * t - 6.2e-6 * t * t * t;
        seconds = std::fmod(seconds, 86400.0);
        if (seconds < 0.0) {
            seconds += 86400.0;
        }
        CHECK(pointing::gmst_degrees(at_unix(row.unix_seconds)) / 15.0 ==
              Approx(seconds / 3600.0).epsilon(0).margin(3e-6));
    }
}

// Falsified by: gmst_degrees() truncating the time point to whole seconds (the
// half-second case below steps by 0.5 s of sidereal rate, 7.5 arcsec).
TEST_CASE("pointing - gmst_degrees keeps sub-second resolution", "[util][pointing]") {
    const auto whole = at_unix(946728000);
    const auto half = whole + std::chrono::milliseconds(500);
    const double step = pointing::gmst_degrees(half) - pointing::gmst_degrees(whole);
    CHECK(step == Approx(0.5 * 360.98564736629 / 86400.0).epsilon(1e-3));
}

// Falsified by: local_sidereal_hours() adding the longitude in hours instead of
// degrees, subtracting it, or not wrapping into [0, 24).
TEST_CASE("pointing - local_sidereal_hours adds the east longitude and wraps into the 0 to 24 hour range",
          "[util][pointing]") {
    const auto j2000 = at_unix(946728000);
    const double greenwich = 280.46061837 / 15.0;
    CHECK(pointing::local_sidereal_hours(j2000, 0.0) == Approx(greenwich).epsilon(0).margin(1e-9));
    CHECK(pointing::local_sidereal_hours(j2000, 15.0) == Approx(greenwich + 1.0).epsilon(0).margin(1e-9));
    CHECK(pointing::local_sidereal_hours(j2000, -151.215297) ==
          Approx(greenwich - 151.215297 / 15.0).epsilon(0).margin(1e-9));
    for (double longitude : {-180.0, -90.0, 0.0, 90.0, 180.0}) {
        const double lst = pointing::local_sidereal_hours(j2000, longitude);
        CHECK(lst >= 0.0);
        CHECK(lst < 24.0);
    }
}

// Falsified by: wrap_hours() using a single fmod without the negative branch, or without the >= 24 guard,
// or returning 24.0 for an input of 24.
TEST_CASE("pointing - wrap_hours lands in the 0 to 24 hour range", "[util][pointing]") {
    CHECK(pointing::wrap_hours(0.0) == 0.0);
    CHECK(pointing::wrap_hours(24.0) == 0.0);
    CHECK(pointing::wrap_hours(-0.5) == Approx(23.5));
    CHECK(pointing::wrap_hours(25.5) == Approx(1.5));
    CHECK(pointing::wrap_hours(-24.0) == 0.0);
    // A tiny negative remainder plus 24.0 rounds to exactly 24.0; the guard folds it back to 0.
    CHECK(pointing::wrap_hours(-1e-18) == 0.0);
}

// Falsified by: wrap_degrees() using a single fmod without the negative branch, or without the >= 360 guard.
TEST_CASE("pointing - wrap_degrees lands in the 0 to 360 degree range", "[util][pointing]") {
    CHECK(pointing::wrap_degrees(0.0) == 0.0);
    CHECK(pointing::wrap_degrees(360.0) == 0.0);
    CHECK(pointing::wrap_degrees(-90.0) == Approx(270.0));
    CHECK(pointing::wrap_degrees(450.0) == Approx(90.0));
    CHECK(pointing::wrap_degrees(-180.0) == Approx(180.0));
    CHECK(pointing::wrap_degrees(-1e-15) == 0.0);
}

// Falsified by: wrap_hour_angle() closing the interval on the positive side
// ((-12, 12], the old ZWO convention): +12 would stay +12.
TEST_CASE("pointing - wrap_hour_angle lands in the minus 12 to 12 hour range", "[util][pointing]") {
    CHECK(pointing::wrap_hour_angle(0.0) == 0.0);
    CHECK(pointing::wrap_hour_angle(12.0) == Approx(-12.0));
    CHECK(pointing::wrap_hour_angle(-12.0) == Approx(-12.0));
    CHECK(pointing::wrap_hour_angle(11.5) == Approx(11.5));
    CHECK(pointing::wrap_hour_angle(13.0) == Approx(-11.0));
    CHECK(pointing::wrap_hour_angle(-13.0) == Approx(11.0));
    CHECK(pointing::wrap_hour_angle(36.0) == Approx(-12.0));
}

// Falsified by: shortest_ra_delta_hours() with the arguments swapped (sign
// flips), or without the 24 h wrap (0.5 - 23.5 stays -23).
TEST_CASE("pointing - shortest_ra_delta_hours takes the short way round", "[util][pointing]") {
    CHECK(pointing::shortest_ra_delta_hours(5.0, 3.0) == Approx(2.0));
    CHECK(pointing::shortest_ra_delta_hours(3.0, 5.0) == Approx(-2.0));
    CHECK(pointing::shortest_ra_delta_hours(0.5, 23.5) == Approx(1.0));
    CHECK(pointing::shortest_ra_delta_hours(23.5, 0.5) == Approx(-1.0));
    CHECK(pointing::shortest_ra_delta_hours(7.0, 7.0) == 0.0);
}

// Falsified by: shortest_ra_delta_hours() wrapping at 12 with < instead of <=
// (an exact half turn flips sign), or a loop-free wrap that drops +-12.
TEST_CASE("pointing - shortest_ra_delta_hours keeps plus or minus 12 at the half turn", "[util][pointing]") {
    CHECK(pointing::shortest_ra_delta_hours(12.0, 0.0) == Approx(12.0));
    CHECK(pointing::shortest_ra_delta_hours(0.0, 12.0) == Approx(-12.0));
    CHECK(pointing::shortest_ra_delta_hours(23.99, 0.0) == Approx(-0.01).epsilon(0).margin(1e-9));
}

// Falsified by: pier_side_for_hour_angle() testing > 0 instead of >= 0 (HA 0
// would be west), or returning the sides swapped.
TEST_CASE("pointing - pier_side_for_hour_angle is 0 for HA at or above 0 and 1 below 0", "[util][pointing]") {
    CHECK(pointing::pier_side_for_hour_angle(0.0) == 0);
    CHECK(pointing::pier_side_for_hour_angle(6.0) == 0);
    CHECK(pointing::pier_side_for_hour_angle(-0.0001) == 1);
    CHECK(pointing::pier_side_for_hour_angle(-12.0) == 1);
}

// Falsified by: validate_ra() accepting 24 (<= instead of <), dropping the
// isfinite test (NaN passes), or changing the message text.
TEST_CASE("pointing - validate_ra accepts 0 up to 24 and throws InvalidValue with the label", "[util][pointing]") {
    CHECK_NOTHROW(pointing::validate_ra(0.0, "TargetRightAscension"));
    CHECK_NOTHROW(pointing::validate_ra(23.999, "TargetRightAscension"));
    const std::string expected = "TargetRightAscension must be in range 0 to <24 hours";
    require_invalid_value([] { pointing::validate_ra(24.0, "TargetRightAscension"); }, expected);
    require_invalid_value([] { pointing::validate_ra(-0.001, "TargetRightAscension"); }, expected);
    require_invalid_value(
        [] { pointing::validate_ra(std::numeric_limits<double>::quiet_NaN(), "TargetRightAscension"); }, expected);
    require_invalid_value(
        [] { pointing::validate_ra(std::numeric_limits<double>::infinity(), "TargetRightAscension"); }, expected);
}

// Falsified by: validate_dec() accepting +-90.0001 (< instead of <=), dropping
// the isfinite test, or changing the message text.
TEST_CASE("pointing - validate_dec accepts minus 90 to 90 and throws InvalidValue with the label", "[util][pointing]") {
    CHECK_NOTHROW(pointing::validate_dec(-90.0, "TargetDeclination"));
    CHECK_NOTHROW(pointing::validate_dec(90.0, "TargetDeclination"));
    const std::string expected = "TargetDeclination must be in range -90 to 90 degrees";
    require_invalid_value([] { pointing::validate_dec(90.0001, "TargetDeclination"); }, expected);
    require_invalid_value([] { pointing::validate_dec(-90.0001, "TargetDeclination"); }, expected);
    require_invalid_value([] { pointing::validate_dec(std::numeric_limits<double>::quiet_NaN(), "TargetDeclination"); },
                          expected);
}

// Falsified by: validate_ra_dec() checking only one of the two, checking Dec
// before RA, or dropping the label from either message.
TEST_CASE("pointing - validate_ra_dec checks RA first and names the label", "[util][pointing]") {
    CHECK_NOTHROW(pointing::validate_ra_dec(12.0, 45.0, "SlewToCoordinates"));
    require_invalid_value([] { pointing::validate_ra_dec(24.0, 0.0, "SlewToCoordinates"); },
                          "SlewToCoordinates: RightAscension must be in range 0 to <24 hours");
    require_invalid_value([] { pointing::validate_ra_dec(1.0, 91.0, "SlewToCoordinates"); },
                          "SlewToCoordinates: Declination must be in range -90 to 90 degrees");
    require_invalid_value([] { pointing::validate_ra_dec(25.0, 91.0, "Park"); },
                          "Park: RightAscension must be in range 0 to <24 hours");
}

// Falsified by: validate_latitude()/validate_longitude() swapping the limits
// (90 vs 180), accepting NaN, or changing the message text.
TEST_CASE("pointing - validate_latitude and validate_longitude throw InvalidValue", "[util][pointing]") {
    CHECK_NOTHROW(pointing::validate_latitude(-90.0));
    CHECK_NOTHROW(pointing::validate_latitude(90.0));
    CHECK_NOTHROW(pointing::validate_longitude(-180.0));
    CHECK_NOTHROW(pointing::validate_longitude(180.0));
    require_invalid_value([] { pointing::validate_latitude(90.5); }, "SiteLatitude must be in range -90 to 90 degrees");
    require_invalid_value([] { pointing::validate_latitude(std::numeric_limits<double>::quiet_NaN()); },
                          "SiteLatitude must be in range -90 to 90 degrees");
    require_invalid_value([] { pointing::validate_longitude(-180.5); },
                          "SiteLongitude must be in range -180 to 180 degrees");
    require_invalid_value([] { pointing::validate_longitude(181.0); },
                          "SiteLongitude must be in range -180 to 180 degrees");
}

// Falsified by: validate_site() checking only one coordinate, or longitude
// before latitude.
TEST_CASE("pointing - validate_site checks latitude then longitude", "[util][pointing]") {
    CHECK_NOTHROW(pointing::validate_site(-33.856784, 151.215297));
    require_invalid_value([] { pointing::validate_site(95.0, 200.0); },
                          "SiteLatitude must be in range -90 to 90 degrees");
    require_invalid_value([] { pointing::validate_site(51.477833, -200.0); },
                          "SiteLongitude must be in range -180 to 180 degrees");
}

namespace {

using pointing::GemPierLimits;
using pointing::GemPierModel;
using pointing::GemPierPreference;
using pointing::GemPointing;

constexpr double kMin20 = 20.0 / 60.0;  // 20 minutes in hours

// Meridian limits 60/60 minutes and RA-axis limits +/-180 degrees: the rig mount of #2179.
GemPierLimits rig_limits() { return GemPierLimits{1.0, 1.0, -180.0, 180.0}; }

GemPierModel model_of(GemPierPreference preference, GemPierLimits limits = rig_limits(), bool southern = false,
                      bool flips_enabled = true) {
    return GemPierModel{preference, flips_enabled, limits, southern};
}

GemPointing at_side(int side, double ha_hours, double dec_degrees = 30.0) {
    return GemPointing{side, false, ha_hours, dec_degrees};
}

GemPointing at_home() { return GemPointing{-1, true, 6.0, 90.0}; }

struct PierCase {
    const char* name;
    GemPierModel model;
    GemPointing current;
    double target_ha_hours;
    double target_dec_degrees;
    int expected;
};

}  // namespace

// The five TeSeek HM-17 steps of #2179 (OnStepX 10.23a, Dec 30, limits 60/60 min, preferred side Best unless
// noted): the mount stays on its side past the meridian up to the limit, a preferred side flips, and past the
// limit the other side takes over.
// Falsified by: gem_destination_pier_side() answering from the hour-angle sign (steps B and E, and Best past the
// west limit), or ignoring the preference (step D).
TEST_CASE("pointing - the GEM pier-side model reproduces the OnStepX rig steps", "[util][pointing][pierside]") {
    const auto best = model_of(GemPierPreference::Best);
    const std::vector<PierCase> cases = {
        {"A: Best at home to HA -20 min is West", best, at_home(), -kMin20, 30.0, 1},
        {"B: Best on West to HA +20 min stays West", best, at_side(1, -0.5), kMin20, 30.0, 1},
        {"D: East preferred on West to HA +20 min flips East", model_of(GemPierPreference::East), at_side(1, -0.5),
         kMin20, 30.0, 0},
        {"E: Best on East to HA -20 min stays East", best, at_side(0, 0.5), -kMin20, 30.0, 0},
        {"Best on West to HA +2 h, past the 1 h west limit, takes East", best, at_side(1, 0.5), 2.0, 30.0, 0},
        {"Best on East to HA -2 h, past the 1 h east limit, takes West", best, at_side(0, -0.5), -2.0, 30.0, 1},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        CHECK(pointing::gem_destination_pier_side(c.model, c.current, c.target_ha_hours, c.target_dec_degrees) ==
              c.expected);
    }
}

// Falsified by: the preference table in gem_destination_pier_side() mapping West or Auto to the wrong start side,
// or Auto using the target's hour angle instead of the current one.
TEST_CASE("pointing - the GEM pier-side model follows West and Auto preferences", "[util][pointing][pierside]") {
    const auto west = model_of(GemPierPreference::West);
    const auto automatic = model_of(GemPierPreference::Auto);
    const std::vector<PierCase> cases = {
        {"West preferred flips an east mount to West", west, at_side(0, 0.5), 0.5, 30.0, 1},
        {"West preferred, target east of the meridian, stays West", west, at_side(0, 0.5), -3.0, 30.0, 1},
        {"West preferred falls back to East past the west limit", west, at_side(0, 0.5), 2.0, 30.0, 0},
        {"Auto past 6 h of current hour angle starts West", automatic, at_side(0, 7.0), 0.5, 30.0, 1},
        {"Auto past 6 h to the west also starts West", automatic, at_side(1, -7.0), 0.5, 30.0, 1},
        {"Auto within 6 h starts East", automatic, at_side(0, 3.0), 0.5, 30.0, 0},
        {"Auto within 6 h falls back to West when East cannot reach", automatic, at_side(0, 3.0), -2.0, 30.0, 1},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        CHECK(pointing::gem_destination_pier_side(c.model, c.current, c.target_ha_hours, c.target_dec_degrees) ==
              c.expected);
    }
}

// At home the side follows the target's hour angle whatever the preference, then the distance rule applies.
// Falsified by: the at-home branch using the preference (East would answer 0 for HA -20 min).
TEST_CASE("pointing - the GEM pier-side model at home follows the target hour angle", "[util][pointing][pierside]") {
    for (const auto preference :
         {GemPierPreference::East, GemPierPreference::West, GemPierPreference::Auto, GemPierPreference::Best}) {
        CHECK(pointing::gem_destination_pier_side(model_of(preference), at_home(), -kMin20, 30.0) == 1);
        CHECK(pointing::gem_destination_pier_side(model_of(preference), at_home(), kMin20, 30.0) == 0);
    }
}

// Falsified by: the flips-disabled branch trying the west side, or answering 0 for a target east cannot reach.
TEST_CASE("pointing - the GEM pier-side model with flips disabled uses the east side only",
          "[util][pointing][pierside]") {
    const auto off = model_of(GemPierPreference::Best, rig_limits(), false, false);
    CHECK(pointing::gem_destination_pier_side(off, at_side(1, 0.5), kMin20, 30.0) == 0);
    CHECK(pointing::gem_destination_pier_side(off, at_side(0, 0.5), 2.0, 30.0) == 0);
    CHECK(pointing::gem_destination_pier_side(off, at_side(0, 0.5), -2.0, 30.0) == -1);
    CHECK(pointing::gem_destination_pier_side(off, at_home(), -kMin20, 30.0) == 0);
}

// Falsified by: the limit comparisons being exclusive, or the east and west limits being swapped.
TEST_CASE("pointing - gem_pier_side_reaches honours the meridian limits inclusively", "[util][pointing][pierside]") {
    const auto limits = rig_limits();
    CHECK(pointing::gem_pier_side_reaches(limits, 0, -1.0));
    CHECK_FALSE(pointing::gem_pier_side_reaches(limits, 0, -1.001));
    CHECK(pointing::gem_pier_side_reaches(limits, 0, 11.9));
    CHECK(pointing::gem_pier_side_reaches(limits, 1, 1.0));
    CHECK_FALSE(pointing::gem_pier_side_reaches(limits, 1, 1.001));
    CHECK(pointing::gem_pier_side_reaches(limits, 1, -11.9));
    // The hour angle wraps into [-12, 12) first.
    CHECK(pointing::gem_pier_side_reaches(limits, 1, 24.5));
    CHECK_FALSE(pointing::gem_pier_side_reaches(limits, 1, 25.5));
    // Asymmetric limits: 30 minutes east, 2 hours west.
    const GemPierLimits lopsided{0.5, 2.0, -180.0, 180.0};
    CHECK(pointing::gem_pier_side_reaches(lopsided, 0, -0.5));
    CHECK_FALSE(pointing::gem_pier_side_reaches(lopsided, 0, -0.6));
    CHECK(pointing::gem_pier_side_reaches(lopsided, 1, 2.0));
    CHECK_FALSE(pointing::gem_pier_side_reaches(lopsided, 1, 2.1));
}

// A limit of 0 reaches the meridian and no further; a negative limit pulls the side back from the meridian.
// Falsified by: the limit being clamped at 0, or its sign being dropped.
TEST_CASE("pointing - the GEM pier-side model handles zero and negative meridian limits",
          "[util][pointing][pierside]") {
    const GemPierLimits zero{0.0, 0.0, -180.0, 180.0};
    const auto best_zero = model_of(GemPierPreference::Best, zero);
    // East reaches HA >= 0 only: at HA -6 min the mount takes West; at HA +6 min West cannot, East does.
    CHECK(pointing::gem_destination_pier_side(best_zero, at_side(0, 0.2), -0.1, 30.0) == 1);
    CHECK(pointing::gem_destination_pier_side(best_zero, at_side(1, -0.2), 0.1, 30.0) == 0);
    CHECK(pointing::gem_pier_side_reaches(zero, 0, 0.0));
    CHECK(pointing::gem_pier_side_reaches(zero, 1, 0.0));

    // East limit -1 h: East reaches HA >= +1 h only. West limit 1 h.
    const GemPierLimits east_negative{-1.0, 1.0, -180.0, 180.0};
    CHECK_FALSE(pointing::gem_pier_side_reaches(east_negative, 0, 0.5));
    CHECK(pointing::gem_pier_side_reaches(east_negative, 0, 1.0));
    CHECK(pointing::gem_destination_pier_side(model_of(GemPierPreference::Best, east_negative), at_side(0, 2.0), 0.5,
                                              30.0) == 1);
    // West limit -1 h: West reaches HA <= -1 h only.
    const GemPierLimits west_negative{1.0, -1.0, -180.0, 180.0};
    CHECK_FALSE(pointing::gem_pier_side_reaches(west_negative, 1, -0.5));
    CHECK(pointing::gem_destination_pier_side(model_of(GemPierPreference::Best, west_negative), at_side(1, -2.0), 0.5,
                                              30.0) == 0);
}

// Falsified by: the axis limits being ignored (the first and last rows) or applied to the wrong side.
TEST_CASE("pointing - the GEM pier-side model honours narrowed RA-axis limits", "[util][pointing][pierside]") {
    const GemPierLimits narrow{1.0, 1.0, -90.0, 90.0};
    const auto best = model_of(GemPierPreference::Best, narrow);
    // East reaches HA -1 h to +6 h; West reaches HA -6 h to +1 h.
    CHECK(pointing::gem_pier_side_reaches(narrow, 0, 5.5));
    CHECK_FALSE(pointing::gem_pier_side_reaches(narrow, 0, 6.5));
    CHECK(pointing::gem_pier_side_reaches(narrow, 1, -5.5));
    CHECK_FALSE(pointing::gem_pier_side_reaches(narrow, 1, -6.5));
    // Best on West, target HA +5.5 h: only East reaches it.
    CHECK(pointing::gem_destination_pier_side(best, at_side(1, 0.0), 5.5, 30.0) == 0);
    // Best on West, target HA -3 h: only West reaches it.
    CHECK(pointing::gem_destination_pier_side(best, at_side(1, 0.0), -3.0, 30.0) == 1);
    // Neither side reaches HA 8 h or HA -7 h.
    CHECK(pointing::gem_destination_pier_side(best, at_side(1, 0.0), 8.0, 30.0) == -1);
    CHECK(pointing::gem_destination_pier_side(best, at_side(0, 0.0), -7.0, 30.0) == -1);
}

// RA-axis limits beyond +/-180 degrees let a side reach an hour angle through the +/-360 degree shift, and the
// distance uses the shifted reading. Limits 10/10 h, axis +/-270: East reaches HA -11 h as 195 degrees.
// Current East at HA +10 h (150 degrees, Dec 30): East dist = |195 - 150| = 45; West target is 15 degrees with
// Dec -> 150: dist = max(|15 - 150|, |150 - 30|) = 135. Without the shift East would be 315 away and West win.
// Falsified by: the +/-360 shift being dropped from reach or from the distance.
TEST_CASE("pointing - the GEM pier-side model shifts axis 1 by 360 degrees when the limits allow",
          "[util][pointing][pierside]") {
    const GemPierLimits wide{10.0, 10.0, -270.0, 270.0};
    CHECK(pointing::gem_pier_side_reaches(wide, 0, -11.0));
    CHECK_FALSE(pointing::gem_pier_side_reaches(rig_limits(), 0, -11.0));
    CHECK(pointing::gem_destination_pier_side(model_of(GemPierPreference::Best, wide), at_side(0, 10.0), -11.0, 30.0) ==
          0);
}

// Best takes the other side when it needs strictly less axis travel. Current West at HA -6 h, Dec 85, target HA +1 h,
// Dec 85, limits 60/60 min: West is at 90 degrees / 95 and reaches 195 / 95 (the west limit itself): dist =
// max(|195 - 90|, 0) = 105. East reaches 15 / 85: dist = max(|15 - 90|, |85 - 95|) = 75. East is closer.
// Falsified by: the distance comparison being dropped, or using the sum of the axes.
TEST_CASE("pointing - the GEM pier-side model Best takes the other side when it is closer",
          "[util][pointing][pierside]") {
    const auto best = model_of(GemPierPreference::Best);
    CHECK(pointing::gem_destination_pier_side(best, at_side(1, -6.0, 85.0), 1.0, 85.0) == 0);
    // Not for an explicit preference: West stays West.
    CHECK(pointing::gem_destination_pier_side(model_of(GemPierPreference::West), at_side(1, -6.0, 85.0), 1.0, 85.0) ==
          1);
    // A target on the spot stays on the current side.
    CHECK(pointing::gem_destination_pier_side(best, at_side(0, 0.0, 0.0), 0.0, 0.0) == 0);
}

// Southern hemisphere: the west side's declination axis reads -180 - Dec. Dec -30 at home (axis 2 = -90), target
// HA -20 min: West is 175 / -150, dist = max(85, 60) = 85; East is -5 / -30, dist = max(95, 60) = 95 -> West. With
// the northern formula West would be 175 / 210 and East would win.
// Falsified by: the southern flag being ignored.
TEST_CASE("pointing - the GEM pier-side model has a southern-hemisphere west axis", "[util][pointing][pierside]") {
    const auto southern_best = model_of(GemPierPreference::Best, rig_limits(), true);
    CHECK(pointing::gem_destination_pier_side(southern_best, at_home(), -kMin20, -30.0) == 1);
    CHECK(pointing::gem_destination_pier_side(southern_best, at_side(1, -0.5, -30.0), kMin20, -30.0) == 1);
    CHECK(pointing::gem_destination_pier_side(southern_best, at_side(0, 0.5, -30.0), -kMin20, -30.0) == 0);
    CHECK(pointing::gem_destination_pier_side(southern_best, at_side(1, 0.5, -30.0), 2.0, -30.0) == 0);
}
