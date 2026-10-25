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
